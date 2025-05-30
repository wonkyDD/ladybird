/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <AK/NumericLimits.h>
#include <AK/SIMDExtras.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/AbstractMachine/Operators.h>
#include <LibWasm/Opcode.h>
#include <LibWasm/Printer/Printer.h>

using namespace AK::SIMD;

namespace Wasm {

#define TRAP_IF_NOT(x)                                                                         \
    do {                                                                                       \
        if (trap_if_not(x, #x##sv)) {                                                          \
            dbgln_if(WASM_TRACE_DEBUG, "Trapped because {} failed, at line {}", #x, __LINE__); \
            return;                                                                            \
        }                                                                                      \
    } while (false)

void BytecodeInterpreter::interpret(Configuration& configuration)
{
    m_trap = Empty {};
    auto& instructions = configuration.frame().expression().instructions();
    auto max_ip_value = InstructionPointer { instructions.size() };
    auto& current_ip_value = configuration.ip();
    auto const should_limit_instruction_count = configuration.should_limit_instruction_count();
    u64 executed_instructions = 0;

    while (current_ip_value < max_ip_value) {
        if (should_limit_instruction_count) {
            if (executed_instructions++ >= Constants::max_allowed_executed_instructions_per_call) [[unlikely]] {
                m_trap = Trap::from_string("Exceeded maximum allowed number of instructions");
                return;
            }
        }
        auto& instruction = instructions[current_ip_value.value()];
        auto old_ip = current_ip_value;
        interpret_instruction(configuration, current_ip_value, instruction);
        if (did_trap())
            return;
        if (current_ip_value == old_ip) // If no jump occurred
            ++current_ip_value;
    }
}

void BytecodeInterpreter::branch_to_label(Configuration& configuration, LabelIndex index)
{
    dbgln_if(WASM_TRACE_DEBUG, "Branch to label with index {}...", index.value());
    for (size_t i = 0; i < index.value(); ++i)
        configuration.label_stack().take_last();
    auto label = configuration.label_stack().last();
    dbgln_if(WASM_TRACE_DEBUG, "...which is actually IP {}, and has {} result(s)", label.continuation().value(), label.arity());

    configuration.value_stack().remove(label.stack_height(), configuration.value_stack().size() - label.stack_height() - label.arity());
    configuration.ip() = label.continuation();
}

template<typename ReadType, typename PushType>
void BytecodeInterpreter::load_and_push(Configuration& configuration, Instruction const& instruction)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    auto& entry = configuration.value_stack().last();
    auto base = entry.to<i32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + arg.offset;
    if (instance_address + sizeof(ReadType) > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln("LibWasm: Memory access out of bounds (expected {} to be less than or equal to {})", instance_address + sizeof(ReadType), memory->size());
        return;
    }
    dbgln_if(WASM_TRACE_DEBUG, "load({} : {}) -> stack", instance_address, sizeof(ReadType));
    auto slice = memory->data().bytes().slice(instance_address, sizeof(ReadType));
    entry = Value(static_cast<PushType>(read_value<ReadType>(slice)));
}

template<typename TDst, typename TSrc>
ALWAYS_INLINE static TDst convert_vector(TSrc v)
{
    return __builtin_convertvector(v, TDst);
}

template<size_t M, size_t N, template<typename> typename SetSign>
void BytecodeInterpreter::load_and_push_mxn(Configuration& configuration, Instruction const& instruction)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    auto& entry = configuration.value_stack().last();
    auto base = entry.to<i32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + arg.offset;
    if (instance_address + M * N / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln("LibWasm: Memory access out of bounds (expected {} to be less than or equal to {})", instance_address + M * N / 8, memory->size());
        return;
    }
    dbgln_if(WASM_TRACE_DEBUG, "vec-load({} : {}) -> stack", instance_address, M * N / 8);
    auto slice = memory->data().bytes().slice(instance_address, M * N / 8);
    using V64 = NativeVectorType<M, N, SetSign>;
    using V128 = NativeVectorType<M * 2, N, SetSign>;

    V64 bytes { 0 };
    if (bit_cast<FlatPtr>(slice.data()) % sizeof(V64) == 0)
        bytes = *bit_cast<V64*>(slice.data());
    else
        ByteReader::load(slice.data(), bytes);

    entry = Value(bit_cast<u128>(convert_vector<V128>(bytes)));
}

template<size_t N>
void BytecodeInterpreter::load_and_push_lane_n(Configuration& configuration, Instruction const& instruction)
{
    auto memarg_and_lane = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    auto& address = configuration.frame().module().memories()[memarg_and_lane.memory.memory_index.value()];
    auto memory = configuration.store().get(address);
    auto vector = configuration.value_stack().take_last().to<u128>();
    auto base = configuration.value_stack().take_last().to<u32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + memarg_and_lane.memory.offset;
    if (instance_address + N / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        return;
    }
    auto slice = memory->data().bytes().slice(instance_address, N / 8);
    auto dst = bit_cast<u8*>(&vector) + memarg_and_lane.lane * N / 8;
    memcpy(dst, slice.data(), N / 8);
    configuration.value_stack().append(Value(vector));
}

template<size_t N>
void BytecodeInterpreter::load_and_push_zero_n(Configuration& configuration, Instruction const& instruction)
{
    auto memarg_and_lane = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[memarg_and_lane.memory_index.value()];
    auto memory = configuration.store().get(address);
    auto base = configuration.value_stack().take_last().to<u32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + memarg_and_lane.offset;
    if (instance_address + N / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        return;
    }
    auto slice = memory->data().bytes().slice(instance_address, N / 8);
    u128 vector = 0;
    memcpy(&vector, slice.data(), N / 8);
    configuration.value_stack().append(Value(vector));
}

template<size_t M>
void BytecodeInterpreter::load_and_push_m_splat(Configuration& configuration, Instruction const& instruction)
{
    auto& arg = instruction.arguments().get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    auto& entry = configuration.value_stack().last();
    auto base = entry.to<i32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + arg.offset;
    if (instance_address + M / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln("LibWasm: Memory access out of bounds (expected {} to be less than or equal to {})", instance_address + M / 8, memory->size());
        return;
    }
    dbgln_if(WASM_TRACE_DEBUG, "vec-splat({} : {}) -> stack", instance_address, M / 8);
    auto slice = memory->data().bytes().slice(instance_address, M / 8);
    auto value = read_value<NativeIntegralType<M>>(slice);
    set_top_m_splat<M, NativeIntegralType>(configuration, value);
}

template<size_t M, template<size_t> typename NativeType>
void BytecodeInterpreter::set_top_m_splat(Wasm::Configuration& configuration, NativeType<M> value)
{
    auto push = [&](auto result) {
        configuration.value_stack().last() = Value(bit_cast<u128>(result));
    };

    if constexpr (IsFloatingPoint<NativeType<32>>) {
        if constexpr (M == 32) // 32 -> 32x4
            push(expand4(value));
        else if constexpr (M == 64) // 64 -> 64x2
            push(f64x2 { value, value });
        else
            static_assert(DependentFalse<NativeType<M>>, "Invalid vector size");
    } else {
        if constexpr (M == 8) // 8 -> 8x4 -> 32x4
            push(expand4(bit_cast<u32>(u8x4 { value, value, value, value })));
        else if constexpr (M == 16) // 16 -> 16x2 -> 32x4
            push(expand4(bit_cast<u32>(u16x2 { value, value })));
        else if constexpr (M == 32) // 32 -> 32x4
            push(expand4(value));
        else if constexpr (M == 64) // 64 -> 64x2
            push(u64x2 { value, value });
        else
            static_assert(DependentFalse<NativeType<M>>, "Invalid vector size");
    }
}

template<size_t M, template<size_t> typename NativeType>
void BytecodeInterpreter::pop_and_push_m_splat(Wasm::Configuration& configuration, Instruction const&)
{
    using PopT = Conditional<M <= 32, NativeType<32>, NativeType<64>>;
    using ReadT = NativeType<M>;
    auto entry = configuration.value_stack().last();
    auto value = static_cast<ReadT>(entry.to<PopT>());
    dbgln_if(WASM_TRACE_DEBUG, "stack({}) -> splat({})", value, M);
    set_top_m_splat<M, NativeType>(configuration, value);
}

template<typename M, template<typename> typename SetSign, typename VectorType>
VectorType BytecodeInterpreter::pop_vector(Configuration& configuration)
{
    return bit_cast<VectorType>(configuration.value_stack().take_last().to<u128>());
}

void BytecodeInterpreter::call_address(Configuration& configuration, FunctionAddress address)
{
    TRAP_IF_NOT(m_stack_info.size_free() >= Constants::minimum_stack_space_to_keep_free);

    auto instance = configuration.store().get(address);
    FunctionType const* type { nullptr };
    instance->visit([&](auto const& function) { type = &function.type(); });
    Vector<Value> args;
    args.ensure_capacity(type->parameters().size());
    auto span = configuration.value_stack().span().slice_from_end(type->parameters().size());
    for (auto& value : span)
        args.unchecked_append(value);

    configuration.value_stack().remove(configuration.value_stack().size() - span.size(), span.size());

    Result result { Trap::from_string("") };
    if (instance->has<WasmFunction>()) {
        CallFrameHandle handle { *this, configuration };
        result = configuration.call(*this, address, move(args));
    } else {
        result = configuration.call(*this, address, move(args));
    }

    if (result.is_trap()) {
        m_trap = move(result.trap());
        return;
    }

    configuration.value_stack().ensure_capacity(configuration.value_stack().size() + result.values().size());
    for (auto& entry : result.values().in_reverse())
        configuration.value_stack().unchecked_append(entry);
}

template<typename PopTypeLHS, typename PushType, typename Operator, typename PopTypeRHS, typename... Args>
void BytecodeInterpreter::binary_numeric_operation(Configuration& configuration, Args&&... args)
{
    auto rhs = configuration.value_stack().take_last().to<PopTypeRHS>();
    auto& lhs_entry = configuration.value_stack().last();
    auto lhs = lhs_entry.to<PopTypeLHS>();
    PushType result;
    auto call_result = Operator { forward<Args>(args)... }(lhs, rhs);
    if constexpr (IsSpecializationOf<decltype(call_result), AK::ErrorOr>) {
        if (call_result.is_error()) {
            trap_if_not(false, call_result.error());
            return;
        }
        result = call_result.release_value();
    } else {
        result = call_result;
    }
    dbgln_if(WASM_TRACE_DEBUG, "{} {} {} = {}", lhs.value(), Operator::name(), rhs.value(), result);
    lhs_entry = Value(result);
}

template<typename PopType, typename PushType, typename Operator, typename... Args>
void BytecodeInterpreter::unary_operation(Configuration& configuration, Args&&... args)
{
    auto& entry = configuration.value_stack().last();
    auto value = entry.to<PopType>();
    auto call_result = Operator { forward<Args>(args)... }(value);
    PushType result;
    if constexpr (IsSpecializationOf<decltype(call_result), AK::ErrorOr>) {
        if (call_result.is_error()) {
            trap_if_not(false, call_result.error());
            return;
        }
        result = call_result.release_value();
    } else {
        result = call_result;
    }
    dbgln_if(WASM_TRACE_DEBUG, "map({}) {} = {}", Operator::name(), *value, result);
    entry = Value(result);
}

template<typename T>
struct ConvertToRaw {
    T operator()(T value)
    {
        return LittleEndian<T>(value);
    }
};

template<>
struct ConvertToRaw<float> {
    u32 operator()(float value)
    {
        ReadonlyBytes bytes { &value, sizeof(float) };
        FixedMemoryStream stream { bytes };
        auto res = stream.read_value<LittleEndian<u32>>().release_value_but_fixme_should_propagate_errors();
        return static_cast<u32>(res);
    }
};

template<>
struct ConvertToRaw<double> {
    u64 operator()(double value)
    {
        ReadonlyBytes bytes { &value, sizeof(double) };
        FixedMemoryStream stream { bytes };
        auto res = stream.read_value<LittleEndian<u64>>().release_value_but_fixme_should_propagate_errors();
        return static_cast<u64>(res);
    }
};

template<typename PopT, typename StoreT>
void BytecodeInterpreter::pop_and_store(Configuration& configuration, Instruction const& instruction)
{
    auto& memarg = instruction.arguments().get<Instruction::MemoryArgument>();
    auto entry = configuration.value_stack().take_last();
    auto value = ConvertToRaw<StoreT> {}(entry.to<PopT>());
    dbgln_if(WASM_TRACE_DEBUG, "stack({}) -> temporary({}b)", value, sizeof(StoreT));
    auto base = configuration.value_stack().take_last().to<i32>();
    store_to_memory(configuration, memarg, { &value, sizeof(StoreT) }, base);
}

template<size_t N>
void BytecodeInterpreter::pop_and_store_lane_n(Configuration& configuration, Instruction const& instruction)
{
    auto& memarg_and_lane = instruction.arguments().get<Instruction::MemoryAndLaneArgument>();
    auto vector = configuration.value_stack().take_last().to<u128>();
    auto src = bit_cast<u8*>(&vector) + memarg_and_lane.lane * N / 8;
    auto base = configuration.value_stack().take_last().to<u32>();
    store_to_memory(configuration, memarg_and_lane.memory, { src, N / 8 }, base);
}

void BytecodeInterpreter::store_to_memory(Configuration& configuration, Instruction::MemoryArgument const& arg, ReadonlyBytes data, u32 base)
{
    auto& address = configuration.frame().module().memories()[arg.memory_index.value()];
    auto memory = configuration.store().get(address);
    u64 instance_address = static_cast<u64>(base) + arg.offset;
    Checked addition { instance_address };
    addition += data.size();
    if (addition.has_overflow() || addition.value() > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln("LibWasm: Memory access out of bounds (expected 0 <= {} and {} <= {})", instance_address, instance_address + data.size(), memory->size());
        return;
    }
    dbgln_if(WASM_TRACE_DEBUG, "temporary({}b) -> store({})", data.size(), instance_address);
    data.copy_to(memory->data().bytes().slice(instance_address, data.size()));
}

template<typename T>
T BytecodeInterpreter::read_value(ReadonlyBytes data)
{
    FixedMemoryStream stream { data };
    auto value_or_error = stream.read_value<LittleEndian<T>>();
    if (value_or_error.is_error()) {
        dbgln("Read from {} failed", data.data());
        m_trap = Trap::from_string("Read from memory failed");
    }
    return value_or_error.release_value();
}

template<>
float BytecodeInterpreter::read_value<float>(ReadonlyBytes data)
{
    FixedMemoryStream stream { data };
    auto raw_value_or_error = stream.read_value<LittleEndian<u32>>();
    if (raw_value_or_error.is_error())
        m_trap = Trap::from_string("Read from memory failed");
    auto raw_value = raw_value_or_error.release_value();
    return bit_cast<float>(static_cast<u32>(raw_value));
}

template<>
double BytecodeInterpreter::read_value<double>(ReadonlyBytes data)
{
    FixedMemoryStream stream { data };
    auto raw_value_or_error = stream.read_value<LittleEndian<u64>>();
    if (raw_value_or_error.is_error())
        m_trap = Trap::from_string("Read from memory failed");
    auto raw_value = raw_value_or_error.release_value();
    return bit_cast<double>(static_cast<u64>(raw_value));
}

ALWAYS_INLINE void BytecodeInterpreter::interpret_instruction(Configuration& configuration, InstructionPointer& ip, Instruction const& instruction)
{
    dbgln_if(WASM_TRACE_DEBUG, "Executing instruction {} at ip {}", instruction_name(instruction.opcode()), ip.value());

    switch (instruction.opcode().value()) {
    case Instructions::unreachable.value():
        m_trap = Trap::from_string("Unreachable");
        return;
    case Instructions::nop.value():
        return;
    case Instructions::local_get.value():
        configuration.value_stack().append(Value(configuration.frame().locals()[instruction.arguments().get<LocalIndex>().value()]));
        return;
    case Instructions::local_set.value(): {
        auto value = configuration.value_stack().take_last();
        configuration.frame().locals()[instruction.arguments().get<LocalIndex>().value()] = value;
        return;
    }
    case Instructions::i32_const.value():
        configuration.value_stack().append(Value(instruction.arguments().get<i32>()));
        return;
    case Instructions::i64_const.value():
        configuration.value_stack().append(Value(instruction.arguments().get<i64>()));
        return;
    case Instructions::f32_const.value():
        configuration.value_stack().append(Value(instruction.arguments().get<float>()));
        return;
    case Instructions::f64_const.value():
        configuration.value_stack().append(Value(instruction.arguments().get<double>()));
        return;
    case Instructions::block.value(): {
        size_t arity = 0;
        size_t param_arity = 0;
        auto& args = instruction.arguments().get<Instruction::StructuredInstructionArgs>();
        switch (args.block_type.kind()) {
        case BlockType::Empty:
            break;
        case BlockType::Type:
            arity = 1;
            break;
        case BlockType::Index: {
            auto& type = configuration.frame().module().types()[args.block_type.type_index().value()];
            arity = type.results().size();
            param_arity = type.parameters().size();
        }
        }

        configuration.label_stack().append(Label(arity, args.end_ip, configuration.value_stack().size() - param_arity));
        return;
    }
    case Instructions::loop.value(): {
        auto& args = instruction.arguments().get<Instruction::StructuredInstructionArgs>();
        size_t arity = 0;
        if (args.block_type.kind() == BlockType::Index) {
            auto& type = configuration.frame().module().types()[args.block_type.type_index().value()];
            arity = type.parameters().size();
        }
        configuration.label_stack().append(Label(arity, ip.value() + 1, configuration.value_stack().size() - arity));
        return;
    }
    case Instructions::if_.value(): {
        size_t arity = 0;
        size_t param_arity = 0;
        auto& args = instruction.arguments().get<Instruction::StructuredInstructionArgs>();
        switch (args.block_type.kind()) {
        case BlockType::Empty:
            break;
        case BlockType::Type:
            arity = 1;
            break;
        case BlockType::Index: {
            auto& type = configuration.frame().module().types()[args.block_type.type_index().value()];
            arity = type.results().size();
            param_arity = type.parameters().size();
        }
        }

        auto value = configuration.value_stack().take_last().to<i32>();
        auto end_label = Label(arity, args.end_ip.value(), configuration.value_stack().size() - param_arity);
        if (value == 0) {
            if (args.else_ip.has_value()) {
                configuration.ip() = args.else_ip.value();
                configuration.label_stack().append(end_label);
            } else {
                configuration.ip() = args.end_ip.value() + 1;
            }
        } else {
            configuration.label_stack().append(end_label);
        }
        return;
    }
    case Instructions::structured_end.value():
        configuration.label_stack().take_last();
        return;
    case Instructions::structured_else.value(): {
        auto label = configuration.label_stack().take_last();
        // Jump to the end label
        configuration.ip() = label.continuation();
        return;
    }
    case Instructions::return_.value(): {
        while (configuration.label_stack().size() - 1 != configuration.frame().label_index())
            configuration.label_stack().take_last();
        configuration.ip() = configuration.frame().expression().instructions().size();
        return;
    }
    case Instructions::br.value():
        return branch_to_label(configuration, instruction.arguments().get<LabelIndex>());
    case Instructions::br_if.value(): {
        auto cond = configuration.value_stack().take_last().to<i32>();
        if (cond == 0)
            return;
        return branch_to_label(configuration, instruction.arguments().get<LabelIndex>());
    }
    case Instructions::br_table.value(): {
        auto& arguments = instruction.arguments().get<Instruction::TableBranchArgs>();
        auto i = configuration.value_stack().take_last().to<u32>();

        if (i >= arguments.labels.size()) {
            return branch_to_label(configuration, arguments.default_);
        }
        return branch_to_label(configuration, arguments.labels[i]);
    }
    case Instructions::call.value(): {
        auto index = instruction.arguments().get<FunctionIndex>();
        auto address = configuration.frame().module().functions()[index.value()];
        dbgln_if(WASM_TRACE_DEBUG, "call({})", address.value());
        call_address(configuration, address);
        return;
    }
    case Instructions::call_indirect.value(): {
        auto& args = instruction.arguments().get<Instruction::IndirectCallArgs>();
        auto table_address = configuration.frame().module().tables()[args.table.value()];
        auto table_instance = configuration.store().get(table_address);
        auto index = configuration.value_stack().take_last().to<i32>();
        TRAP_IF_NOT(index >= 0);
        TRAP_IF_NOT(static_cast<size_t>(index) < table_instance->elements().size());
        auto element = table_instance->elements()[index];
        TRAP_IF_NOT(element.ref().has<Reference::Func>());
        auto address = element.ref().get<Reference::Func>().address;
        dbgln_if(WASM_TRACE_DEBUG, "call_indirect({} -> {})", index, address.value());
        call_address(configuration, address);
        return;
    }
    case Instructions::i32_load.value():
        return load_and_push<i32, i32>(configuration, instruction);
    case Instructions::i64_load.value():
        return load_and_push<i64, i64>(configuration, instruction);
    case Instructions::f32_load.value():
        return load_and_push<float, float>(configuration, instruction);
    case Instructions::f64_load.value():
        return load_and_push<double, double>(configuration, instruction);
    case Instructions::i32_load8_s.value():
        return load_and_push<i8, i32>(configuration, instruction);
    case Instructions::i32_load8_u.value():
        return load_and_push<u8, i32>(configuration, instruction);
    case Instructions::i32_load16_s.value():
        return load_and_push<i16, i32>(configuration, instruction);
    case Instructions::i32_load16_u.value():
        return load_and_push<u16, i32>(configuration, instruction);
    case Instructions::i64_load8_s.value():
        return load_and_push<i8, i64>(configuration, instruction);
    case Instructions::i64_load8_u.value():
        return load_and_push<u8, i64>(configuration, instruction);
    case Instructions::i64_load16_s.value():
        return load_and_push<i16, i64>(configuration, instruction);
    case Instructions::i64_load16_u.value():
        return load_and_push<u16, i64>(configuration, instruction);
    case Instructions::i64_load32_s.value():
        return load_and_push<i32, i64>(configuration, instruction);
    case Instructions::i64_load32_u.value():
        return load_and_push<u32, i64>(configuration, instruction);
    case Instructions::i32_store.value():
        return pop_and_store<i32, i32>(configuration, instruction);
    case Instructions::i64_store.value():
        return pop_and_store<i64, i64>(configuration, instruction);
    case Instructions::f32_store.value():
        return pop_and_store<float, float>(configuration, instruction);
    case Instructions::f64_store.value():
        return pop_and_store<double, double>(configuration, instruction);
    case Instructions::i32_store8.value():
        return pop_and_store<i32, i8>(configuration, instruction);
    case Instructions::i32_store16.value():
        return pop_and_store<i32, i16>(configuration, instruction);
    case Instructions::i64_store8.value():
        return pop_and_store<i64, i8>(configuration, instruction);
    case Instructions::i64_store16.value():
        return pop_and_store<i64, i16>(configuration, instruction);
    case Instructions::i64_store32.value():
        return pop_and_store<i64, i32>(configuration, instruction);
    case Instructions::local_tee.value(): {
        auto value = configuration.value_stack().last();
        auto local_index = instruction.arguments().get<LocalIndex>();
        dbgln_if(WASM_TRACE_DEBUG, "stack:peek -> locals({})", local_index.value());
        configuration.frame().locals()[local_index.value()] = value;
        return;
    }
    case Instructions::global_get.value(): {
        auto global_index = instruction.arguments().get<GlobalIndex>();
        // This check here is for const expressions. In non-const expressions,
        // a validation error would have been thrown.
        TRAP_IF_NOT(global_index < configuration.frame().module().globals().size());
        auto address = configuration.frame().module().globals()[global_index.value()];
        dbgln_if(WASM_TRACE_DEBUG, "global({}) -> stack", address.value());
        auto global = configuration.store().get(address);
        configuration.value_stack().append(global->value());
        return;
    }
    case Instructions::global_set.value(): {
        auto global_index = instruction.arguments().get<GlobalIndex>();
        auto address = configuration.frame().module().globals()[global_index.value()];
        auto value = configuration.value_stack().take_last();
        dbgln_if(WASM_TRACE_DEBUG, "stack -> global({})", address.value());
        auto global = configuration.store().get(address);
        global->set_value(value);
        return;
    }
    case Instructions::memory_size.value(): {
        auto& args = instruction.arguments().get<Instruction::MemoryIndexArgument>();
        auto address = configuration.frame().module().memories()[args.memory_index.value()];
        auto instance = configuration.store().get(address);
        auto pages = instance->size() / Constants::page_size;
        dbgln_if(WASM_TRACE_DEBUG, "memory.size -> stack({})", pages);
        configuration.value_stack().append(Value((i32)pages));
        return;
    }
    case Instructions::memory_grow.value(): {
        auto& args = instruction.arguments().get<Instruction::MemoryIndexArgument>();
        auto address = configuration.frame().module().memories()[args.memory_index.value()];
        auto instance = configuration.store().get(address);
        i32 old_pages = instance->size() / Constants::page_size;
        auto& entry = configuration.value_stack().last();
        auto new_pages = entry.to<i32>();
        dbgln_if(WASM_TRACE_DEBUG, "memory.grow({}), previously {} pages...", new_pages, old_pages);
        if (instance->grow(new_pages * Constants::page_size))
            entry = Value((i32)old_pages);
        else
            entry = Value((i32)-1);
        return;
    }
    // https://webassembly.github.io/spec/core/bikeshed/#exec-memory-fill
    case Instructions::memory_fill.value(): {
        auto& args = instruction.arguments().get<Instruction::MemoryIndexArgument>();
        auto address = configuration.frame().module().memories()[args.memory_index.value()];
        auto instance = configuration.store().get(address);
        auto count = configuration.value_stack().take_last().to<u32>();
        u8 value = static_cast<u8>(configuration.value_stack().take_last().to<u32>());
        auto destination_offset = configuration.value_stack().take_last().to<u32>();

        TRAP_IF_NOT(static_cast<size_t>(destination_offset + count) <= instance->data().size());

        if (count == 0)
            return;

        for (u32 i = 0; i < count; ++i)
            store_to_memory(configuration, Instruction::MemoryArgument { 0, 0 }, { &value, sizeof(value) }, destination_offset + i);

        return;
    }
    // https://webassembly.github.io/spec/core/bikeshed/#exec-memory-copy
    case Instructions::memory_copy.value(): {
        auto& args = instruction.arguments().get<Instruction::MemoryCopyArgs>();
        auto source_address = configuration.frame().module().memories()[args.src_index.value()];
        auto destination_address = configuration.frame().module().memories()[args.dst_index.value()];
        auto source_instance = configuration.store().get(source_address);
        auto destination_instance = configuration.store().get(destination_address);

        auto count = configuration.value_stack().take_last().to<i32>();
        auto source_offset = configuration.value_stack().take_last().to<i32>();
        auto destination_offset = configuration.value_stack().take_last().to<i32>();

        Checked<size_t> source_position = source_offset;
        source_position.saturating_add(count);
        Checked<size_t> destination_position = destination_offset;
        destination_position.saturating_add(count);
        TRAP_IF_NOT(source_position <= source_instance->data().size());
        TRAP_IF_NOT(destination_position <= destination_instance->data().size());

        if (count == 0)
            return;

        Instruction::MemoryArgument memarg { 0, 0, args.dst_index };
        if (destination_offset <= source_offset) {
            for (auto i = 0; i < count; ++i) {
                auto value = source_instance->data()[source_offset + i];
                store_to_memory(configuration, memarg, { &value, sizeof(value) }, destination_offset + i);
            }
        } else {
            for (auto i = count - 1; i >= 0; --i) {
                auto value = source_instance->data()[source_offset + i];
                store_to_memory(configuration, memarg, { &value, sizeof(value) }, destination_offset + i);
            }
        }

        return;
    }
    // https://webassembly.github.io/spec/core/bikeshed/#exec-memory-init
    case Instructions::memory_init.value(): {
        auto& args = instruction.arguments().get<Instruction::MemoryInitArgs>();
        auto& data_address = configuration.frame().module().datas()[args.data_index.value()];
        auto& data = *configuration.store().get(data_address);
        auto memory_address = configuration.frame().module().memories()[args.memory_index.value()];
        auto memory = configuration.store().get(memory_address);
        auto count = configuration.value_stack().take_last().to<u32>();
        auto source_offset = configuration.value_stack().take_last().to<u32>();
        auto destination_offset = configuration.value_stack().take_last().to<u32>();

        Checked<size_t> source_position = source_offset;
        source_position.saturating_add(count);
        Checked<size_t> destination_position = destination_offset;
        destination_position.saturating_add(count);
        TRAP_IF_NOT(source_position <= data.data().size());
        TRAP_IF_NOT(destination_position <= memory->data().size());

        if (count == 0)
            return;

        Instruction::MemoryArgument memarg { 0, 0, args.memory_index };
        for (size_t i = 0; i < (size_t)count; ++i) {
            auto value = data.data()[source_offset + i];
            store_to_memory(configuration, memarg, { &value, sizeof(value) }, destination_offset + i);
        }
        return;
    }
    // https://webassembly.github.io/spec/core/bikeshed/#exec-data-drop
    case Instructions::data_drop.value(): {
        auto data_index = instruction.arguments().get<DataIndex>();
        auto data_address = configuration.frame().module().datas()[data_index.value()];
        *configuration.store().get(data_address) = DataInstance({});
        return;
    }
    case Instructions::elem_drop.value(): {
        auto elem_index = instruction.arguments().get<ElementIndex>();
        auto address = configuration.frame().module().elements()[elem_index.value()];
        auto elem = configuration.store().get(address);
        *configuration.store().get(address) = ElementInstance(elem->type(), {});
        return;
    }
    case Instructions::table_init.value(): {
        auto& args = instruction.arguments().get<Instruction::TableElementArgs>();
        auto table_address = configuration.frame().module().tables()[args.table_index.value()];
        auto table = configuration.store().get(table_address);
        auto element_address = configuration.frame().module().elements()[args.element_index.value()];
        auto element = configuration.store().get(element_address);
        auto count = configuration.value_stack().take_last().to<u32>();
        auto source_offset = configuration.value_stack().take_last().to<u32>();
        auto destination_offset = configuration.value_stack().take_last().to<u32>();

        Checked<u32> checked_source_offset = source_offset;
        Checked<u32> checked_destination_offset = destination_offset;
        checked_source_offset += count;
        checked_destination_offset += count;
        TRAP_IF_NOT(!checked_source_offset.has_overflow() && checked_source_offset <= (u32)element->references().size());
        TRAP_IF_NOT(!checked_destination_offset.has_overflow() && checked_destination_offset <= (u32)table->elements().size());

        for (u32 i = 0; i < count; ++i)
            table->elements()[destination_offset + i] = element->references()[source_offset + i];
        return;
    }
    case Instructions::table_copy.value(): {
        auto& args = instruction.arguments().get<Instruction::TableTableArgs>();
        auto source_address = configuration.frame().module().tables()[args.rhs.value()];
        auto destination_address = configuration.frame().module().tables()[args.lhs.value()];
        auto source_instance = configuration.store().get(source_address);
        auto destination_instance = configuration.store().get(destination_address);

        auto count = configuration.value_stack().take_last().to<u32>();
        auto source_offset = configuration.value_stack().take_last().to<u32>();
        auto destination_offset = configuration.value_stack().take_last().to<u32>();

        Checked<size_t> source_position = source_offset;
        source_position.saturating_add(count);
        Checked<size_t> destination_position = destination_offset;
        destination_position.saturating_add(count);
        TRAP_IF_NOT(source_position <= source_instance->elements().size());
        TRAP_IF_NOT(destination_position <= destination_instance->elements().size());

        if (count == 0)
            return;

        if (destination_offset <= source_offset) {
            for (u32 i = 0; i < count; ++i) {
                auto value = source_instance->elements()[source_offset + i];
                destination_instance->elements()[destination_offset + i] = value;
            }
        } else {
            for (u32 i = count - 1; i != NumericLimits<u32>::max(); --i) {
                auto value = source_instance->elements()[source_offset + i];
                destination_instance->elements()[destination_offset + i] = value;
            }
        }

        return;
    }
    case Instructions::table_fill.value(): {
        auto table_index = instruction.arguments().get<TableIndex>();
        auto address = configuration.frame().module().tables()[table_index.value()];
        auto table = configuration.store().get(address);
        auto count = configuration.value_stack().take_last().to<u32>();
        auto value = configuration.value_stack().take_last().to<Reference>();
        auto start = configuration.value_stack().take_last().to<u32>();

        Checked<u32> checked_offset = start;
        checked_offset += count;
        TRAP_IF_NOT(!checked_offset.has_overflow() && checked_offset <= (u32)table->elements().size());

        for (u32 i = 0; i < count; ++i)
            table->elements()[start + i] = value;
        return;
    }
    case Instructions::table_set.value(): {
        auto ref = configuration.value_stack().take_last().to<Reference>();
        auto index = (size_t)(configuration.value_stack().take_last().to<i32>());
        auto table_index = instruction.arguments().get<TableIndex>();
        auto address = configuration.frame().module().tables()[table_index.value()];
        auto table = configuration.store().get(address);
        TRAP_IF_NOT(index < table->elements().size());
        table->elements()[index] = ref;
        return;
    }
    case Instructions::table_get.value(): {
        auto index = (size_t)(configuration.value_stack().take_last().to<i32>());
        auto table_index = instruction.arguments().get<TableIndex>();
        auto address = configuration.frame().module().tables()[table_index.value()];
        auto table = configuration.store().get(address);
        TRAP_IF_NOT(index < table->elements().size());
        auto ref = table->elements()[index];
        configuration.value_stack().append(Value(ref));
        return;
    }
    case Instructions::table_grow.value(): {
        auto size = configuration.value_stack().take_last().to<u32>();
        auto fill_value = configuration.value_stack().take_last().to<Reference>();
        auto table_index = instruction.arguments().get<TableIndex>();
        auto address = configuration.frame().module().tables()[table_index.value()];
        auto table = configuration.store().get(address);
        auto previous_size = table->elements().size();
        auto did_grow = table->grow(size, fill_value);
        if (!did_grow) {
            configuration.value_stack().append(Value((i32)-1));
        } else {
            configuration.value_stack().append(Value((i32)previous_size));
        }
        return;
    }
    case Instructions::table_size.value(): {
        auto table_index = instruction.arguments().get<TableIndex>();
        auto address = configuration.frame().module().tables()[table_index.value()];
        auto table = configuration.store().get(address);
        configuration.value_stack().append(Value((i32)table->elements().size()));
        return;
    }
    case Instructions::ref_null.value(): {
        auto type = instruction.arguments().get<ValueType>();
        configuration.value_stack().append(Value(Reference(Reference::Null { type })));
        return;
    };
    case Instructions::ref_func.value(): {
        auto index = instruction.arguments().get<FunctionIndex>().value();
        auto& functions = configuration.frame().module().functions();
        auto address = functions[index];
        configuration.value_stack().append(Value(Reference { Reference::Func { address, configuration.store().get_module_for(address) } }));
        return;
    }
    case Instructions::ref_is_null.value(): {
        auto ref = configuration.value_stack().take_last().to<Reference>();
        configuration.value_stack().append(Value(static_cast<i32>(ref.ref().has<Reference::Null>() ? 1 : 0)));
        return;
    }
    case Instructions::drop.value():
        configuration.value_stack().take_last();
        return;
    case Instructions::select.value():
    case Instructions::select_typed.value(): {
        // Note: The type seems to only be used for validation.
        auto value = configuration.value_stack().take_last().to<i32>();
        dbgln_if(WASM_TRACE_DEBUG, "select({})", value);
        auto rhs = configuration.value_stack().take_last();
        auto& lhs = configuration.value_stack().last();
        lhs = value != 0 ? lhs : rhs;
        return;
    }
    case Instructions::i32_eqz.value():
        return unary_operation<i32, i32, Operators::EqualsZero>(configuration);
    case Instructions::i32_eq.value():
        return binary_numeric_operation<i32, i32, Operators::Equals>(configuration);
    case Instructions::i32_ne.value():
        return binary_numeric_operation<i32, i32, Operators::NotEquals>(configuration);
    case Instructions::i32_lts.value():
        return binary_numeric_operation<i32, i32, Operators::LessThan>(configuration);
    case Instructions::i32_ltu.value():
        return binary_numeric_operation<u32, i32, Operators::LessThan>(configuration);
    case Instructions::i32_gts.value():
        return binary_numeric_operation<i32, i32, Operators::GreaterThan>(configuration);
    case Instructions::i32_gtu.value():
        return binary_numeric_operation<u32, i32, Operators::GreaterThan>(configuration);
    case Instructions::i32_les.value():
        return binary_numeric_operation<i32, i32, Operators::LessThanOrEquals>(configuration);
    case Instructions::i32_leu.value():
        return binary_numeric_operation<u32, i32, Operators::LessThanOrEquals>(configuration);
    case Instructions::i32_ges.value():
        return binary_numeric_operation<i32, i32, Operators::GreaterThanOrEquals>(configuration);
    case Instructions::i32_geu.value():
        return binary_numeric_operation<u32, i32, Operators::GreaterThanOrEquals>(configuration);
    case Instructions::i64_eqz.value():
        return unary_operation<i64, i32, Operators::EqualsZero>(configuration);
    case Instructions::i64_eq.value():
        return binary_numeric_operation<i64, i32, Operators::Equals>(configuration);
    case Instructions::i64_ne.value():
        return binary_numeric_operation<i64, i32, Operators::NotEquals>(configuration);
    case Instructions::i64_lts.value():
        return binary_numeric_operation<i64, i32, Operators::LessThan>(configuration);
    case Instructions::i64_ltu.value():
        return binary_numeric_operation<u64, i32, Operators::LessThan>(configuration);
    case Instructions::i64_gts.value():
        return binary_numeric_operation<i64, i32, Operators::GreaterThan>(configuration);
    case Instructions::i64_gtu.value():
        return binary_numeric_operation<u64, i32, Operators::GreaterThan>(configuration);
    case Instructions::i64_les.value():
        return binary_numeric_operation<i64, i32, Operators::LessThanOrEquals>(configuration);
    case Instructions::i64_leu.value():
        return binary_numeric_operation<u64, i32, Operators::LessThanOrEquals>(configuration);
    case Instructions::i64_ges.value():
        return binary_numeric_operation<i64, i32, Operators::GreaterThanOrEquals>(configuration);
    case Instructions::i64_geu.value():
        return binary_numeric_operation<u64, i32, Operators::GreaterThanOrEquals>(configuration);
    case Instructions::f32_eq.value():
        return binary_numeric_operation<float, i32, Operators::Equals>(configuration);
    case Instructions::f32_ne.value():
        return binary_numeric_operation<float, i32, Operators::NotEquals>(configuration);
    case Instructions::f32_lt.value():
        return binary_numeric_operation<float, i32, Operators::LessThan>(configuration);
    case Instructions::f32_gt.value():
        return binary_numeric_operation<float, i32, Operators::GreaterThan>(configuration);
    case Instructions::f32_le.value():
        return binary_numeric_operation<float, i32, Operators::LessThanOrEquals>(configuration);
    case Instructions::f32_ge.value():
        return binary_numeric_operation<float, i32, Operators::GreaterThanOrEquals>(configuration);
    case Instructions::f64_eq.value():
        return binary_numeric_operation<double, i32, Operators::Equals>(configuration);
    case Instructions::f64_ne.value():
        return binary_numeric_operation<double, i32, Operators::NotEquals>(configuration);
    case Instructions::f64_lt.value():
        return binary_numeric_operation<double, i32, Operators::LessThan>(configuration);
    case Instructions::f64_gt.value():
        return binary_numeric_operation<double, i32, Operators::GreaterThan>(configuration);
    case Instructions::f64_le.value():
        return binary_numeric_operation<double, i32, Operators::LessThanOrEquals>(configuration);
    case Instructions::f64_ge.value():
        return binary_numeric_operation<double, i32, Operators::GreaterThanOrEquals>(configuration);
    case Instructions::i32_clz.value():
        return unary_operation<i32, i32, Operators::CountLeadingZeros>(configuration);
    case Instructions::i32_ctz.value():
        return unary_operation<i32, i32, Operators::CountTrailingZeros>(configuration);
    case Instructions::i32_popcnt.value():
        return unary_operation<i32, i32, Operators::PopCount>(configuration);
    case Instructions::i32_add.value():
        return binary_numeric_operation<u32, i32, Operators::Add>(configuration);
    case Instructions::i32_sub.value():
        return binary_numeric_operation<u32, i32, Operators::Subtract>(configuration);
    case Instructions::i32_mul.value():
        return binary_numeric_operation<u32, i32, Operators::Multiply>(configuration);
    case Instructions::i32_divs.value():
        return binary_numeric_operation<i32, i32, Operators::Divide>(configuration);
    case Instructions::i32_divu.value():
        return binary_numeric_operation<u32, i32, Operators::Divide>(configuration);
    case Instructions::i32_rems.value():
        return binary_numeric_operation<i32, i32, Operators::Modulo>(configuration);
    case Instructions::i32_remu.value():
        return binary_numeric_operation<u32, i32, Operators::Modulo>(configuration);
    case Instructions::i32_and.value():
        return binary_numeric_operation<i32, i32, Operators::BitAnd>(configuration);
    case Instructions::i32_or.value():
        return binary_numeric_operation<i32, i32, Operators::BitOr>(configuration);
    case Instructions::i32_xor.value():
        return binary_numeric_operation<i32, i32, Operators::BitXor>(configuration);
    case Instructions::i32_shl.value():
        return binary_numeric_operation<u32, i32, Operators::BitShiftLeft>(configuration);
    case Instructions::i32_shrs.value():
        return binary_numeric_operation<i32, i32, Operators::BitShiftRight>(configuration);
    case Instructions::i32_shru.value():
        return binary_numeric_operation<u32, i32, Operators::BitShiftRight>(configuration);
    case Instructions::i32_rotl.value():
        return binary_numeric_operation<u32, i32, Operators::BitRotateLeft>(configuration);
    case Instructions::i32_rotr.value():
        return binary_numeric_operation<u32, i32, Operators::BitRotateRight>(configuration);
    case Instructions::i64_clz.value():
        return unary_operation<i64, i64, Operators::CountLeadingZeros>(configuration);
    case Instructions::i64_ctz.value():
        return unary_operation<i64, i64, Operators::CountTrailingZeros>(configuration);
    case Instructions::i64_popcnt.value():
        return unary_operation<i64, i64, Operators::PopCount>(configuration);
    case Instructions::i64_add.value():
        return binary_numeric_operation<u64, i64, Operators::Add>(configuration);
    case Instructions::i64_sub.value():
        return binary_numeric_operation<u64, i64, Operators::Subtract>(configuration);
    case Instructions::i64_mul.value():
        return binary_numeric_operation<u64, i64, Operators::Multiply>(configuration);
    case Instructions::i64_divs.value():
        return binary_numeric_operation<i64, i64, Operators::Divide>(configuration);
    case Instructions::i64_divu.value():
        return binary_numeric_operation<u64, i64, Operators::Divide>(configuration);
    case Instructions::i64_rems.value():
        return binary_numeric_operation<i64, i64, Operators::Modulo>(configuration);
    case Instructions::i64_remu.value():
        return binary_numeric_operation<u64, i64, Operators::Modulo>(configuration);
    case Instructions::i64_and.value():
        return binary_numeric_operation<i64, i64, Operators::BitAnd>(configuration);
    case Instructions::i64_or.value():
        return binary_numeric_operation<i64, i64, Operators::BitOr>(configuration);
    case Instructions::i64_xor.value():
        return binary_numeric_operation<i64, i64, Operators::BitXor>(configuration);
    case Instructions::i64_shl.value():
        return binary_numeric_operation<u64, i64, Operators::BitShiftLeft>(configuration);
    case Instructions::i64_shrs.value():
        return binary_numeric_operation<i64, i64, Operators::BitShiftRight>(configuration);
    case Instructions::i64_shru.value():
        return binary_numeric_operation<u64, i64, Operators::BitShiftRight>(configuration);
    case Instructions::i64_rotl.value():
        return binary_numeric_operation<u64, i64, Operators::BitRotateLeft>(configuration);
    case Instructions::i64_rotr.value():
        return binary_numeric_operation<u64, i64, Operators::BitRotateRight>(configuration);
    case Instructions::f32_abs.value():
        return unary_operation<float, float, Operators::Absolute>(configuration);
    case Instructions::f32_neg.value():
        return unary_operation<float, float, Operators::Negate>(configuration);
    case Instructions::f32_ceil.value():
        return unary_operation<float, float, Operators::Ceil>(configuration);
    case Instructions::f32_floor.value():
        return unary_operation<float, float, Operators::Floor>(configuration);
    case Instructions::f32_trunc.value():
        return unary_operation<float, float, Operators::Truncate>(configuration);
    case Instructions::f32_nearest.value():
        return unary_operation<float, float, Operators::NearbyIntegral>(configuration);
    case Instructions::f32_sqrt.value():
        return unary_operation<float, float, Operators::SquareRoot>(configuration);
    case Instructions::f32_add.value():
        return binary_numeric_operation<float, float, Operators::Add>(configuration);
    case Instructions::f32_sub.value():
        return binary_numeric_operation<float, float, Operators::Subtract>(configuration);
    case Instructions::f32_mul.value():
        return binary_numeric_operation<float, float, Operators::Multiply>(configuration);
    case Instructions::f32_div.value():
        return binary_numeric_operation<float, float, Operators::Divide>(configuration);
    case Instructions::f32_min.value():
        return binary_numeric_operation<float, float, Operators::Minimum>(configuration);
    case Instructions::f32_max.value():
        return binary_numeric_operation<float, float, Operators::Maximum>(configuration);
    case Instructions::f32_copysign.value():
        return binary_numeric_operation<float, float, Operators::CopySign>(configuration);
    case Instructions::f64_abs.value():
        return unary_operation<double, double, Operators::Absolute>(configuration);
    case Instructions::f64_neg.value():
        return unary_operation<double, double, Operators::Negate>(configuration);
    case Instructions::f64_ceil.value():
        return unary_operation<double, double, Operators::Ceil>(configuration);
    case Instructions::f64_floor.value():
        return unary_operation<double, double, Operators::Floor>(configuration);
    case Instructions::f64_trunc.value():
        return unary_operation<double, double, Operators::Truncate>(configuration);
    case Instructions::f64_nearest.value():
        return unary_operation<double, double, Operators::NearbyIntegral>(configuration);
    case Instructions::f64_sqrt.value():
        return unary_operation<double, double, Operators::SquareRoot>(configuration);
    case Instructions::f64_add.value():
        return binary_numeric_operation<double, double, Operators::Add>(configuration);
    case Instructions::f64_sub.value():
        return binary_numeric_operation<double, double, Operators::Subtract>(configuration);
    case Instructions::f64_mul.value():
        return binary_numeric_operation<double, double, Operators::Multiply>(configuration);
    case Instructions::f64_div.value():
        return binary_numeric_operation<double, double, Operators::Divide>(configuration);
    case Instructions::f64_min.value():
        return binary_numeric_operation<double, double, Operators::Minimum>(configuration);
    case Instructions::f64_max.value():
        return binary_numeric_operation<double, double, Operators::Maximum>(configuration);
    case Instructions::f64_copysign.value():
        return binary_numeric_operation<double, double, Operators::CopySign>(configuration);
    case Instructions::i32_wrap_i64.value():
        return unary_operation<i64, i32, Operators::Wrap<i32>>(configuration);
    case Instructions::i32_trunc_sf32.value():
        return unary_operation<float, i32, Operators::CheckedTruncate<i32>>(configuration);
    case Instructions::i32_trunc_uf32.value():
        return unary_operation<float, i32, Operators::CheckedTruncate<u32>>(configuration);
    case Instructions::i32_trunc_sf64.value():
        return unary_operation<double, i32, Operators::CheckedTruncate<i32>>(configuration);
    case Instructions::i32_trunc_uf64.value():
        return unary_operation<double, i32, Operators::CheckedTruncate<u32>>(configuration);
    case Instructions::i64_trunc_sf32.value():
        return unary_operation<float, i64, Operators::CheckedTruncate<i64>>(configuration);
    case Instructions::i64_trunc_uf32.value():
        return unary_operation<float, i64, Operators::CheckedTruncate<u64>>(configuration);
    case Instructions::i64_trunc_sf64.value():
        return unary_operation<double, i64, Operators::CheckedTruncate<i64>>(configuration);
    case Instructions::i64_trunc_uf64.value():
        return unary_operation<double, i64, Operators::CheckedTruncate<u64>>(configuration);
    case Instructions::i64_extend_si32.value():
        return unary_operation<i32, i64, Operators::Extend<i64>>(configuration);
    case Instructions::i64_extend_ui32.value():
        return unary_operation<u32, i64, Operators::Extend<i64>>(configuration);
    case Instructions::f32_convert_si32.value():
        return unary_operation<i32, float, Operators::Convert<float>>(configuration);
    case Instructions::f32_convert_ui32.value():
        return unary_operation<u32, float, Operators::Convert<float>>(configuration);
    case Instructions::f32_convert_si64.value():
        return unary_operation<i64, float, Operators::Convert<float>>(configuration);
    case Instructions::f32_convert_ui64.value():
        return unary_operation<u64, float, Operators::Convert<float>>(configuration);
    case Instructions::f32_demote_f64.value():
        return unary_operation<double, float, Operators::Demote>(configuration);
    case Instructions::f64_convert_si32.value():
        return unary_operation<i32, double, Operators::Convert<double>>(configuration);
    case Instructions::f64_convert_ui32.value():
        return unary_operation<u32, double, Operators::Convert<double>>(configuration);
    case Instructions::f64_convert_si64.value():
        return unary_operation<i64, double, Operators::Convert<double>>(configuration);
    case Instructions::f64_convert_ui64.value():
        return unary_operation<u64, double, Operators::Convert<double>>(configuration);
    case Instructions::f64_promote_f32.value():
        return unary_operation<float, double, Operators::Promote>(configuration);
    case Instructions::i32_reinterpret_f32.value():
        return unary_operation<float, i32, Operators::Reinterpret<i32>>(configuration);
    case Instructions::i64_reinterpret_f64.value():
        return unary_operation<double, i64, Operators::Reinterpret<i64>>(configuration);
    case Instructions::f32_reinterpret_i32.value():
        return unary_operation<i32, float, Operators::Reinterpret<float>>(configuration);
    case Instructions::f64_reinterpret_i64.value():
        return unary_operation<i64, double, Operators::Reinterpret<double>>(configuration);
    case Instructions::i32_extend8_s.value():
        return unary_operation<i32, i32, Operators::SignExtend<i8>>(configuration);
    case Instructions::i32_extend16_s.value():
        return unary_operation<i32, i32, Operators::SignExtend<i16>>(configuration);
    case Instructions::i64_extend8_s.value():
        return unary_operation<i64, i64, Operators::SignExtend<i8>>(configuration);
    case Instructions::i64_extend16_s.value():
        return unary_operation<i64, i64, Operators::SignExtend<i16>>(configuration);
    case Instructions::i64_extend32_s.value():
        return unary_operation<i64, i64, Operators::SignExtend<i32>>(configuration);
    case Instructions::i32_trunc_sat_f32_s.value():
        return unary_operation<float, i32, Operators::SaturatingTruncate<i32>>(configuration);
    case Instructions::i32_trunc_sat_f32_u.value():
        return unary_operation<float, i32, Operators::SaturatingTruncate<u32>>(configuration);
    case Instructions::i32_trunc_sat_f64_s.value():
        return unary_operation<double, i32, Operators::SaturatingTruncate<i32>>(configuration);
    case Instructions::i32_trunc_sat_f64_u.value():
        return unary_operation<double, i32, Operators::SaturatingTruncate<u32>>(configuration);
    case Instructions::i64_trunc_sat_f32_s.value():
        return unary_operation<float, i64, Operators::SaturatingTruncate<i64>>(configuration);
    case Instructions::i64_trunc_sat_f32_u.value():
        return unary_operation<float, i64, Operators::SaturatingTruncate<u64>>(configuration);
    case Instructions::i64_trunc_sat_f64_s.value():
        return unary_operation<double, i64, Operators::SaturatingTruncate<i64>>(configuration);
    case Instructions::i64_trunc_sat_f64_u.value():
        return unary_operation<double, i64, Operators::SaturatingTruncate<u64>>(configuration);
    case Instructions::v128_const.value():
        configuration.value_stack().append(Value(instruction.arguments().get<u128>()));
        return;
    case Instructions::v128_load.value():
        return load_and_push<u128, u128>(configuration, instruction);
    case Instructions::v128_load8x8_s.value():
        return load_and_push_mxn<8, 8, MakeSigned>(configuration, instruction);
    case Instructions::v128_load8x8_u.value():
        return load_and_push_mxn<8, 8, MakeUnsigned>(configuration, instruction);
    case Instructions::v128_load16x4_s.value():
        return load_and_push_mxn<16, 4, MakeSigned>(configuration, instruction);
    case Instructions::v128_load16x4_u.value():
        return load_and_push_mxn<16, 4, MakeUnsigned>(configuration, instruction);
    case Instructions::v128_load32x2_s.value():
        return load_and_push_mxn<32, 2, MakeSigned>(configuration, instruction);
    case Instructions::v128_load32x2_u.value():
        return load_and_push_mxn<32, 2, MakeUnsigned>(configuration, instruction);
    case Instructions::v128_load8_splat.value():
        return load_and_push_m_splat<8>(configuration, instruction);
    case Instructions::v128_load16_splat.value():
        return load_and_push_m_splat<16>(configuration, instruction);
    case Instructions::v128_load32_splat.value():
        return load_and_push_m_splat<32>(configuration, instruction);
    case Instructions::v128_load64_splat.value():
        return load_and_push_m_splat<64>(configuration, instruction);
    case Instructions::i8x16_splat.value():
        return pop_and_push_m_splat<8, NativeIntegralType>(configuration, instruction);
    case Instructions::i16x8_splat.value():
        return pop_and_push_m_splat<16, NativeIntegralType>(configuration, instruction);
    case Instructions::i32x4_splat.value():
        return pop_and_push_m_splat<32, NativeIntegralType>(configuration, instruction);
    case Instructions::i64x2_splat.value():
        return pop_and_push_m_splat<64, NativeIntegralType>(configuration, instruction);
    case Instructions::f32x4_splat.value():
        return pop_and_push_m_splat<32, NativeFloatingType>(configuration, instruction);
    case Instructions::f64x2_splat.value():
        return pop_and_push_m_splat<64, NativeFloatingType>(configuration, instruction);
    case Instructions::i8x16_shuffle.value(): {
        auto& arg = instruction.arguments().get<Instruction::ShuffleArgument>();
        auto b = pop_vector<u8, MakeUnsigned>(configuration);
        auto a = pop_vector<u8, MakeUnsigned>(configuration);
        using VectorType = Native128ByteVectorOf<u8, MakeUnsigned>;
        VectorType result;
        for (size_t i = 0; i < 16; ++i)
            if (arg.lanes[i] < 16)
                result[i] = a[arg.lanes[i]];
            else
                result[i] = b[arg.lanes[i] - 16];
        configuration.value_stack().append(Value(bit_cast<u128>(result)));
        return;
    }
    case Instructions::v128_store.value():
        return pop_and_store<u128, u128>(configuration, instruction);
    case Instructions::i8x16_shl.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<16>, i32>(configuration);
    case Instructions::i8x16_shr_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftRight<16, MakeUnsigned>, i32>(configuration);
    case Instructions::i8x16_shr_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftRight<16, MakeSigned>, i32>(configuration);
    case Instructions::i16x8_shl.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<8>, i32>(configuration);
    case Instructions::i16x8_shr_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftRight<8, MakeUnsigned>, i32>(configuration);
    case Instructions::i16x8_shr_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftRight<8, MakeSigned>, i32>(configuration);
    case Instructions::i32x4_shl.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<4>, i32>(configuration);
    case Instructions::i32x4_shr_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftRight<4, MakeUnsigned>, i32>(configuration);
    case Instructions::i32x4_shr_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftRight<4, MakeSigned>, i32>(configuration);
    case Instructions::i64x2_shl.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<2>, i32>(configuration);
    case Instructions::i64x2_shr_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftRight<2, MakeUnsigned>, i32>(configuration);
    case Instructions::i64x2_shr_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorShiftRight<2, MakeSigned>, i32>(configuration);
    case Instructions::i8x16_swizzle.value():
        return binary_numeric_operation<u128, u128, Operators::VectorSwizzle>(configuration);
    case Instructions::i8x16_extract_lane_s.value():
        return unary_operation<u128, i8, Operators::VectorExtractLane<16, MakeSigned>>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::i8x16_extract_lane_u.value():
        return unary_operation<u128, u8, Operators::VectorExtractLane<16, MakeUnsigned>>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::i16x8_extract_lane_s.value():
        return unary_operation<u128, i16, Operators::VectorExtractLane<8, MakeSigned>>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::i16x8_extract_lane_u.value():
        return unary_operation<u128, u16, Operators::VectorExtractLane<8, MakeUnsigned>>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::i32x4_extract_lane.value():
        return unary_operation<u128, i32, Operators::VectorExtractLane<4, MakeSigned>>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::i64x2_extract_lane.value():
        return unary_operation<u128, i64, Operators::VectorExtractLane<2, MakeSigned>>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::f32x4_extract_lane.value():
        return unary_operation<u128, float, Operators::VectorExtractLaneFloat<4>>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::f64x2_extract_lane.value():
        return unary_operation<u128, double, Operators::VectorExtractLaneFloat<2>>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::i8x16_replace_lane.value():
        return binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<16, i32>, i32>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::i16x8_replace_lane.value():
        return binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<8, i32>, i32>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::i32x4_replace_lane.value():
        return binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<4>, i32>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::i64x2_replace_lane.value():
        return binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<2>, i64>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::f32x4_replace_lane.value():
        return binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<4, float>, float>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::f64x2_replace_lane.value():
        return binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<2, double>, double>(configuration, instruction.arguments().get<Instruction::LaneIndex>().lane);
    case Instructions::i8x16_eq.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::Equals>>(configuration);
    case Instructions::i8x16_ne.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::NotEquals>>(configuration);
    case Instructions::i8x16_lt_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThan, MakeSigned>>(configuration);
    case Instructions::i8x16_lt_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThan, MakeUnsigned>>(configuration);
    case Instructions::i8x16_gt_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThan, MakeSigned>>(configuration);
    case Instructions::i8x16_gt_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThan, MakeUnsigned>>(configuration);
    case Instructions::i8x16_le_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThanOrEquals, MakeSigned>>(configuration);
    case Instructions::i8x16_le_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThanOrEquals, MakeUnsigned>>(configuration);
    case Instructions::i8x16_ge_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThanOrEquals, MakeSigned>>(configuration);
    case Instructions::i8x16_ge_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThanOrEquals, MakeUnsigned>>(configuration);
    case Instructions::i8x16_abs.value():
        return unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::Absolute>>(configuration);
    case Instructions::i8x16_neg.value():
        return unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::Negate>>(configuration);
    case Instructions::i8x16_all_true.value():
        return unary_operation<u128, i32, Operators::VectorAllTrue<16>>(configuration);
    case Instructions::i8x16_popcnt.value():
        return unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::PopCount>>(configuration);
    case Instructions::i8x16_add.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Add>>(configuration);
    case Instructions::i8x16_sub.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Subtract>>(configuration);
    case Instructions::i8x16_avgr_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Average, MakeUnsigned>>(configuration);
    case Instructions::i8x16_add_sat_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<i8, Operators::Add>, MakeSigned>>(configuration);
    case Instructions::i8x16_add_sat_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<u8, Operators::Add>, MakeUnsigned>>(configuration);
    case Instructions::i8x16_sub_sat_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<i8, Operators::Subtract>, MakeSigned>>(configuration);
    case Instructions::i8x16_sub_sat_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<u8, Operators::Subtract>, MakeUnsigned>>(configuration);
    case Instructions::i8x16_min_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Minimum, MakeSigned>>(configuration);
    case Instructions::i8x16_min_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Minimum, MakeUnsigned>>(configuration);
    case Instructions::i8x16_max_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Maximum, MakeSigned>>(configuration);
    case Instructions::i8x16_max_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Maximum, MakeUnsigned>>(configuration);
    case Instructions::i16x8_eq.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::Equals>>(configuration);
    case Instructions::i16x8_ne.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::NotEquals>>(configuration);
    case Instructions::i16x8_lt_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThan, MakeSigned>>(configuration);
    case Instructions::i16x8_lt_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThan, MakeUnsigned>>(configuration);
    case Instructions::i16x8_gt_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThan, MakeSigned>>(configuration);
    case Instructions::i16x8_gt_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThan, MakeUnsigned>>(configuration);
    case Instructions::i16x8_le_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThanOrEquals, MakeSigned>>(configuration);
    case Instructions::i16x8_le_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThanOrEquals, MakeUnsigned>>(configuration);
    case Instructions::i16x8_ge_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThanOrEquals, MakeSigned>>(configuration);
    case Instructions::i16x8_ge_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThanOrEquals, MakeUnsigned>>(configuration);
    case Instructions::i16x8_abs.value():
        return unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<8, Operators::Absolute>>(configuration);
    case Instructions::i16x8_neg.value():
        return unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<8, Operators::Negate>>(configuration);
    case Instructions::i16x8_all_true.value():
        return unary_operation<u128, i32, Operators::VectorAllTrue<8>>(configuration);
    case Instructions::i16x8_add.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Add>>(configuration);
    case Instructions::i16x8_sub.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Subtract>>(configuration);
    case Instructions::i16x8_mul.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Multiply>>(configuration);
    case Instructions::i16x8_avgr_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Average, MakeUnsigned>>(configuration);
    case Instructions::i16x8_add_sat_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Add>, MakeSigned>>(configuration);
    case Instructions::i16x8_add_sat_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<u16, Operators::Add>, MakeUnsigned>>(configuration);
    case Instructions::i16x8_sub_sat_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Subtract>, MakeSigned>>(configuration);
    case Instructions::i16x8_sub_sat_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<u16, Operators::Subtract>, MakeUnsigned>>(configuration);
    case Instructions::i16x8_min_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Minimum, MakeSigned>>(configuration);
    case Instructions::i16x8_min_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Minimum, MakeUnsigned>>(configuration);
    case Instructions::i16x8_max_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Maximum, MakeSigned>>(configuration);
    case Instructions::i16x8_max_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Maximum, MakeUnsigned>>(configuration);
    case Instructions::i16x8_extend_low_i8x16_s.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::Low, MakeSigned>>(configuration);
    case Instructions::i16x8_extend_high_i8x16_s.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::High, MakeSigned>>(configuration);
    case Instructions::i16x8_extend_low_i8x16_u.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::Low, MakeUnsigned>>(configuration);
    case Instructions::i16x8_extend_high_i8x16_u.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::High, MakeUnsigned>>(configuration);
    case Instructions::i16x8_extadd_pairwise_i8x16_s.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<8, Operators::Add, MakeSigned>>(configuration);
    case Instructions::i16x8_extadd_pairwise_i8x16_u.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<8, Operators::Add, MakeUnsigned>>(configuration);
    case Instructions::i16x8_extmul_low_i8x16_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>>(configuration);
    case Instructions::i16x8_extmul_high_i8x16_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::High, MakeSigned>>(configuration);
    case Instructions::i16x8_extmul_low_i8x16_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>>(configuration);
    case Instructions::i16x8_extmul_high_i8x16_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>>(configuration);
    case Instructions::i32x4_eq.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::Equals>>(configuration);
    case Instructions::i32x4_ne.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::NotEquals>>(configuration);
    case Instructions::i32x4_lt_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThan, MakeSigned>>(configuration);
    case Instructions::i32x4_lt_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThan, MakeUnsigned>>(configuration);
    case Instructions::i32x4_gt_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThan, MakeSigned>>(configuration);
    case Instructions::i32x4_gt_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThan, MakeUnsigned>>(configuration);
    case Instructions::i32x4_le_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThanOrEquals, MakeSigned>>(configuration);
    case Instructions::i32x4_le_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThanOrEquals, MakeUnsigned>>(configuration);
    case Instructions::i32x4_ge_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThanOrEquals, MakeSigned>>(configuration);
    case Instructions::i32x4_ge_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThanOrEquals, MakeUnsigned>>(configuration);
    case Instructions::i32x4_abs.value():
        return unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<4, Operators::Absolute>>(configuration);
    case Instructions::i32x4_neg.value():
        return unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<4, Operators::Negate, MakeUnsigned>>(configuration);
    case Instructions::i32x4_all_true.value():
        return unary_operation<u128, i32, Operators::VectorAllTrue<4>>(configuration);
    case Instructions::i32x4_add.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Add, MakeUnsigned>>(configuration);
    case Instructions::i32x4_sub.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Subtract, MakeUnsigned>>(configuration);
    case Instructions::i32x4_mul.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Multiply, MakeUnsigned>>(configuration);
    case Instructions::i32x4_min_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Minimum, MakeSigned>>(configuration);
    case Instructions::i32x4_min_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Minimum, MakeUnsigned>>(configuration);
    case Instructions::i32x4_max_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Maximum, MakeSigned>>(configuration);
    case Instructions::i32x4_max_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Maximum, MakeUnsigned>>(configuration);
    case Instructions::i32x4_extend_low_i16x8_s.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::Low, MakeSigned>>(configuration);
    case Instructions::i32x4_extend_high_i16x8_s.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::High, MakeSigned>>(configuration);
    case Instructions::i32x4_extend_low_i16x8_u.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::Low, MakeUnsigned>>(configuration);
    case Instructions::i32x4_extend_high_i16x8_u.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::High, MakeUnsigned>>(configuration);
    case Instructions::i32x4_extadd_pairwise_i16x8_s.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<4, Operators::Add, MakeSigned>>(configuration);
    case Instructions::i32x4_extadd_pairwise_i16x8_u.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<4, Operators::Add, MakeUnsigned>>(configuration);
    case Instructions::i32x4_extmul_low_i16x8_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>>(configuration);
    case Instructions::i32x4_extmul_high_i16x8_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::High, MakeSigned>>(configuration);
    case Instructions::i32x4_extmul_low_i16x8_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>>(configuration);
    case Instructions::i32x4_extmul_high_i16x8_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>>(configuration);
    case Instructions::i64x2_eq.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::Equals>>(configuration);
    case Instructions::i64x2_ne.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::NotEquals>>(configuration);
    case Instructions::i64x2_lt_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::LessThan, MakeSigned>>(configuration);
    case Instructions::i64x2_gt_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::GreaterThan, MakeSigned>>(configuration);
    case Instructions::i64x2_le_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::LessThanOrEquals, MakeSigned>>(configuration);
    case Instructions::i64x2_ge_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::GreaterThanOrEquals, MakeSigned>>(configuration);
    case Instructions::i64x2_abs.value():
        return unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<2, Operators::Absolute>>(configuration);
    case Instructions::i64x2_neg.value():
        return unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<2, Operators::Negate, MakeUnsigned>>(configuration);
    case Instructions::i64x2_all_true.value():
        return unary_operation<u128, i32, Operators::VectorAllTrue<2>>(configuration);
    case Instructions::i64x2_add.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Add, MakeUnsigned>>(configuration);
    case Instructions::i64x2_sub.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Subtract, MakeUnsigned>>(configuration);
    case Instructions::i64x2_mul.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Multiply, MakeUnsigned>>(configuration);
    case Instructions::i64x2_extend_low_i32x4_s.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::Low, MakeSigned>>(configuration);
    case Instructions::i64x2_extend_high_i32x4_s.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::High, MakeSigned>>(configuration);
    case Instructions::i64x2_extend_low_i32x4_u.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::Low, MakeUnsigned>>(configuration);
    case Instructions::i64x2_extend_high_i32x4_u.value():
        return unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::High, MakeUnsigned>>(configuration);
    case Instructions::i64x2_extmul_low_i32x4_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>>(configuration);
    case Instructions::i64x2_extmul_high_i32x4_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::High, MakeSigned>>(configuration);
    case Instructions::i64x2_extmul_low_i32x4_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>>(configuration);
    case Instructions::i64x2_extmul_high_i32x4_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>>(configuration);
    case Instructions::f32x4_eq.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::Equals>>(configuration);
    case Instructions::f32x4_ne.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::NotEquals>>(configuration);
    case Instructions::f32x4_lt.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::LessThan>>(configuration);
    case Instructions::f32x4_gt.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::GreaterThan>>(configuration);
    case Instructions::f32x4_le.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::LessThanOrEquals>>(configuration);
    case Instructions::f32x4_ge.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::GreaterThanOrEquals>>(configuration);
    case Instructions::f32x4_min.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Minimum>>(configuration);
    case Instructions::f32x4_max.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Maximum>>(configuration);
    case Instructions::f64x2_eq.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::Equals>>(configuration);
    case Instructions::f64x2_ne.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::NotEquals>>(configuration);
    case Instructions::f64x2_lt.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::LessThan>>(configuration);
    case Instructions::f64x2_gt.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::GreaterThan>>(configuration);
    case Instructions::f64x2_le.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::LessThanOrEquals>>(configuration);
    case Instructions::f64x2_ge.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::GreaterThanOrEquals>>(configuration);
    case Instructions::f64x2_min.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Minimum>>(configuration);
    case Instructions::f64x2_max.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Maximum>>(configuration);
    case Instructions::f32x4_div.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Divide>>(configuration);
    case Instructions::f32x4_mul.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Multiply>>(configuration);
    case Instructions::f32x4_sub.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Subtract>>(configuration);
    case Instructions::f32x4_add.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Add>>(configuration);
    case Instructions::f32x4_pmin.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::PseudoMinimum>>(configuration);
    case Instructions::f32x4_pmax.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::PseudoMaximum>>(configuration);
    case Instructions::f64x2_div.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Divide>>(configuration);
    case Instructions::f64x2_mul.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Multiply>>(configuration);
    case Instructions::f64x2_sub.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Subtract>>(configuration);
    case Instructions::f64x2_add.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Add>>(configuration);
    case Instructions::f64x2_pmin.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::PseudoMinimum>>(configuration);
    case Instructions::f64x2_pmax.value():
        return binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::PseudoMaximum>>(configuration);
    case Instructions::f32x4_ceil.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Ceil>>(configuration);
    case Instructions::f32x4_floor.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Floor>>(configuration);
    case Instructions::f32x4_trunc.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Truncate>>(configuration);
    case Instructions::f32x4_nearest.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::NearbyIntegral>>(configuration);
    case Instructions::f32x4_sqrt.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::SquareRoot>>(configuration);
    case Instructions::f32x4_neg.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Negate>>(configuration);
    case Instructions::f32x4_abs.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Absolute>>(configuration);
    case Instructions::f64x2_ceil.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Ceil>>(configuration);
    case Instructions::f64x2_floor.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Floor>>(configuration);
    case Instructions::f64x2_trunc.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Truncate>>(configuration);
    case Instructions::f64x2_nearest.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::NearbyIntegral>>(configuration);
    case Instructions::f64x2_sqrt.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::SquareRoot>>(configuration);
    case Instructions::f64x2_neg.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Negate>>(configuration);
    case Instructions::f64x2_abs.value():
        return unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Absolute>>(configuration);
    case Instructions::v128_and.value():
        return binary_numeric_operation<u128, u128, Operators::BitAnd>(configuration);
    case Instructions::v128_or.value():
        return binary_numeric_operation<u128, u128, Operators::BitOr>(configuration);
    case Instructions::v128_xor.value():
        return binary_numeric_operation<u128, u128, Operators::BitXor>(configuration);
    case Instructions::v128_not.value():
        return unary_operation<u128, u128, Operators::BitNot>(configuration);
    case Instructions::v128_andnot.value():
        return binary_numeric_operation<u128, u128, Operators::BitAndNot>(configuration);
    case Instructions::v128_bitselect.value(): {
        auto mask = configuration.value_stack().take_last().to<u128>();
        auto false_vector = configuration.value_stack().take_last().to<u128>();
        auto true_vector = configuration.value_stack().take_last().to<u128>();
        u128 result = (true_vector & mask) | (false_vector & ~mask);
        configuration.value_stack().append(Value(result));
        return;
    }
    case Instructions::v128_any_true.value(): {
        auto vector = configuration.value_stack().take_last().to<u128>();
        configuration.value_stack().append(Value(static_cast<i32>(vector != 0)));
        return;
    }
    case Instructions::v128_load8_lane.value():
        return load_and_push_lane_n<8>(configuration, instruction);
    case Instructions::v128_load16_lane.value():
        return load_and_push_lane_n<16>(configuration, instruction);
    case Instructions::v128_load32_lane.value():
        return load_and_push_lane_n<32>(configuration, instruction);
    case Instructions::v128_load64_lane.value():
        return load_and_push_lane_n<64>(configuration, instruction);
    case Instructions::v128_load32_zero.value():
        return load_and_push_zero_n<32>(configuration, instruction);
    case Instructions::v128_load64_zero.value():
        return load_and_push_zero_n<64>(configuration, instruction);
    case Instructions::v128_store8_lane.value():
        return pop_and_store_lane_n<8>(configuration, instruction);
    case Instructions::v128_store16_lane.value():
        return pop_and_store_lane_n<16>(configuration, instruction);
    case Instructions::v128_store32_lane.value():
        return pop_and_store_lane_n<32>(configuration, instruction);
    case Instructions::v128_store64_lane.value():
        return pop_and_store_lane_n<64>(configuration, instruction);
    case Instructions::i32x4_trunc_sat_f32x4_s.value():
        return unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, f32, Operators::SaturatingTruncate<i32>>>(configuration);
    case Instructions::i32x4_trunc_sat_f32x4_u.value():
        return unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, f32, Operators::SaturatingTruncate<u32>>>(configuration);
    case Instructions::i8x16_bitmask.value():
        return unary_operation<u128, i32, Operators::VectorBitmask<16>>(configuration);
    case Instructions::i16x8_bitmask.value():
        return unary_operation<u128, i32, Operators::VectorBitmask<8>>(configuration);
    case Instructions::i32x4_bitmask.value():
        return unary_operation<u128, i32, Operators::VectorBitmask<4>>(configuration);
    case Instructions::i64x2_bitmask.value():
        return unary_operation<u128, i32, Operators::VectorBitmask<2>>(configuration);
    case Instructions::i32x4_dot_i16x8_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorDotProduct<4>>(configuration);
    case Instructions::i8x16_narrow_i16x8_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorNarrow<16, i8>>(configuration);
    case Instructions::i8x16_narrow_i16x8_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorNarrow<16, u8>>(configuration);
    case Instructions::i16x8_narrow_i32x4_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorNarrow<8, i16>>(configuration);
    case Instructions::i16x8_narrow_i32x4_u.value():
        return binary_numeric_operation<u128, u128, Operators::VectorNarrow<8, u16>>(configuration);
    case Instructions::i16x8_q15mulr_sat_s.value():
        return binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Q15Mul>, MakeSigned>>(configuration);
    case Instructions::f32x4_convert_i32x4_s.value():
        return unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, i32, Operators::Convert<f32>>>(configuration);
    case Instructions::f32x4_convert_i32x4_u.value():
        return unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, u32, Operators::Convert<f32>>>(configuration);
    case Instructions::f64x2_convert_low_i32x4_s.value():
        return unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, i32, Operators::Convert<f64>>>(configuration);
    case Instructions::f64x2_convert_low_i32x4_u.value():
        return unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, u32, Operators::Convert<f64>>>(configuration);
    case Instructions::f32x4_demote_f64x2_zero.value():
        return unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::Convert<f32>>>(configuration);
    case Instructions::f64x2_promote_low_f32x4.value():
        return unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, f32, Operators::Convert<f64>>>(configuration);
    case Instructions::i32x4_trunc_sat_f64x2_s_zero.value():
        return unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::SaturatingTruncate<i32>>>(configuration);
    case Instructions::i32x4_trunc_sat_f64x2_u_zero.value():
        return unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::SaturatingTruncate<u32>>>(configuration);
    }
}

void DebuggerBytecodeInterpreter::interpret_instruction(Configuration& configuration, InstructionPointer& ip, Instruction const& instruction)
{
    if (pre_interpret_hook) {
        auto result = pre_interpret_hook(configuration, ip, instruction);
        if (!result) {
            m_trap = Trap::from_string("Trapped by user request");
            return;
        }
    }

    BytecodeInterpreter::interpret_instruction(configuration, ip, instruction);

    if (post_interpret_hook) {
        auto result = post_interpret_hook(configuration, ip, instruction, *this);
        if (!result) {
            m_trap = Trap::from_string("Trapped by user request");
            return;
        }
    }
}
}
