/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/RegExpStringIterator.h>

namespace JS {

GC_DEFINE_ALLOCATOR(RegExpStringIterator);

// 22.2.9.1 CreateRegExpStringIterator ( R, S, global, fullUnicode ), https://tc39.es/ecma262/#sec-createregexpstringiterator
GC::Ref<RegExpStringIterator> RegExpStringIterator::create(Realm& realm, Object& regexp_object, GC::Ref<PrimitiveString> string, bool global, bool unicode)
{
    // 1. Let iterator be OrdinaryObjectCreate(%RegExpStringIteratorPrototype%, « [[IteratingRegExp]], [[IteratedString]], [[Global]], [[Unicode]], [[Done]] »).
    // 2. Set iterator.[[IteratingRegExp]] to R.
    // 3. Set iterator.[[IteratedString]] to S.
    // 4. Set iterator.[[Global]] to global.
    // 5. Set iterator.[[Unicode]] to fullUnicode.
    // 6. Set iterator.[[Done]] to false.
    // 7. Return iterator.
    return realm.create<RegExpStringIterator>(realm.intrinsics().regexp_string_iterator_prototype(), regexp_object, string, global, unicode);
}

RegExpStringIterator::RegExpStringIterator(Object& prototype, Object& regexp_object, GC::Ref<PrimitiveString> string, bool global, bool unicode)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_regexp_object(regexp_object)
    , m_string(string)
    , m_global(global)
    , m_unicode(unicode)
{
}

void RegExpStringIterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_regexp_object);
    visitor.visit(m_string);
}

}
