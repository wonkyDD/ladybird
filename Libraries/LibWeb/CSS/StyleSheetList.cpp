/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StyleSheetListPrototype.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StyleSheetList);

// https://www.w3.org/TR/cssom/#remove-a-css-style-sheet
void StyleSheetList::remove_a_css_style_sheet(CSS::CSSStyleSheet& sheet)
{
    // 1. Remove the CSS style sheet from the list of document or shadow root CSS style sheets.
    remove_sheet(sheet);

    // 2. Set the CSS style sheet’s parent CSS style sheet, owner node and owner CSS rule to null.
    sheet.set_parent_css_style_sheet(nullptr);
    sheet.set_owner_node(nullptr);
    sheet.set_owner_css_rule(nullptr);
}

// https://www.w3.org/TR/cssom/#add-a-css-style-sheet
void StyleSheetList::add_a_css_style_sheet(CSS::CSSStyleSheet& sheet)
{
    // 1. Add the CSS style sheet to the list of document or shadow root CSS style sheets at the appropriate location. The remainder of these steps deal with the disabled flag.
    add_sheet(sheet);

    // 2. If the disabled flag is set, then return.
    if (sheet.disabled())
        return;

    // 3. If the title is not the empty string, the alternate flag is unset, and preferred CSS style sheet set name is the empty string change the preferred CSS style sheet set name to the title.
    if (!sheet.title().is_empty() && !sheet.is_alternate() && m_preferred_css_style_sheet_set_name.is_empty()) {
        m_preferred_css_style_sheet_set_name = sheet.title();
    }

    // 4. If any of the following is true, then unset the disabled flag and return:
    //    - The title is the empty string.
    //    - The last CSS style sheet set name is null and the title is a case-sensitive match for the preferred CSS style sheet set name.
    //    - The title is a case-sensitive match for the last CSS style sheet set name.
    // NOTE: We don't enable alternate sheets with an empty title.  This isn't directly mentioned in the algorithm steps, but the
    // HTML specification says that the title element must be specified with a non-empty value for alternative style sheets.
    // See: https://html.spec.whatwg.org/multipage/links.html#the-link-is-an-alternative-stylesheet
    if ((sheet.title().is_empty() && !sheet.is_alternate())
        || (!m_last_css_style_sheet_set_name.has_value() && sheet.title().equals_ignoring_case(m_preferred_css_style_sheet_set_name))
        || (m_last_css_style_sheet_set_name.has_value() && sheet.title().equals_ignoring_case(m_last_css_style_sheet_set_name.value()))) {
        sheet.set_disabled(false);
        return;
    }

    // 5. Set the disabled flag.
    sheet.set_disabled(true);
}

// https://www.w3.org/TR/cssom/#create-a-css-style-sheet
GC::Ref<CSSStyleSheet> StyleSheetList::create_a_css_style_sheet(String const& css_text, String type, DOM::Element* owner_node, String media, String title, Alternate alternate, OriginClean origin_clean, Optional<::URL::URL> location, CSSStyleSheet* parent_style_sheet, CSSRule* owner_rule)
{
    // 1. Create a new CSS style sheet object and set its properties as specified.
    // AD-HOC: The spec never tells us when to parse this style sheet, but the most logical place is here.
    auto sheet = parse_css_stylesheet(Parser::ParsingParams { document() }, css_text, location);

    sheet->set_parent_css_style_sheet(parent_style_sheet);
    sheet->set_owner_css_rule(owner_rule);
    sheet->set_owner_node(owner_node);
    sheet->set_type(move(type));
    sheet->set_media(move(media));
    sheet->set_title(move(title));
    sheet->set_alternate(alternate == Alternate::Yes);
    sheet->set_origin_clean(origin_clean == OriginClean::Yes);

    // 2. Then run the add a CSS style sheet steps for the newly created CSS style sheet.
    add_a_css_style_sheet(*sheet);

    return sheet;
}

void StyleSheetList::add_sheet(CSSStyleSheet& sheet)
{
    sheet.add_owning_document_or_shadow_root(document_or_shadow_root());

    if (m_sheets.is_empty()) {
        // This is the first sheet, append it to the list.
        m_sheets.append(sheet);
    } else {
        // We have sheets from before. Insert the new sheet in the correct position (DOM tree order).
        bool did_insert = false;
        for (ssize_t i = m_sheets.size() - 1; i >= 0; --i) {
            auto& existing_sheet = *m_sheets[i];
            auto position = existing_sheet.owner_node()->compare_document_position(sheet.owner_node());
            if (position & DOM::Node::DocumentPosition::DOCUMENT_POSITION_FOLLOWING) {
                m_sheets.insert(i + 1, sheet);
                did_insert = true;
                break;
            }
        }
        if (!did_insert)
            m_sheets.prepend(sheet);
    }

    // NOTE: We evaluate media queries immediately when adding a new sheet.
    //       This coalesces the full document style invalidations.
    //       If we don't do this, we invalidate now, and then again when Document updates media rules.
    sheet.evaluate_media_queries(as<HTML::Window>(HTML::relevant_global_object(*this)));

    if (sheet.rules().length() == 0) {
        // NOTE: If the added sheet has no rules, we don't have to invalidate anything.
        return;
    }

    document().style_computer().invalidate_rule_cache();
    document().style_computer().load_fonts_from_sheet(sheet);
    document_or_shadow_root().invalidate_style(DOM::StyleInvalidationReason::StyleSheetListAddSheet);
}

void StyleSheetList::remove_sheet(CSSStyleSheet& sheet)
{
    sheet.remove_owning_document_or_shadow_root(document_or_shadow_root());
    bool did_remove = m_sheets.remove_first_matching([&](auto& entry) { return entry.ptr() == &sheet; });
    VERIFY(did_remove);

    if (sheet.rules().length() == 0) {
        // NOTE: If the removed sheet had no rules, we don't have to invalidate anything.
        return;
    }

    m_document_or_shadow_root->document().style_computer().unload_fonts_from_sheet(sheet);
    m_document_or_shadow_root->document().style_computer().invalidate_rule_cache();
    document_or_shadow_root().invalidate_style(DOM::StyleInvalidationReason::StyleSheetListRemoveSheet);
}

GC::Ref<StyleSheetList> StyleSheetList::create(GC::Ref<DOM::Node> document_or_shadow_root)
{
    auto& realm = document_or_shadow_root->realm();
    return realm.create<StyleSheetList>(document_or_shadow_root);
}

StyleSheetList::StyleSheetList(GC::Ref<DOM::Node> document_or_shadow_root)
    : Bindings::PlatformObject(document_or_shadow_root->realm())
    , m_document_or_shadow_root(document_or_shadow_root)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = true };
}

void StyleSheetList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(StyleSheetList);
    Base::initialize(realm);
}

void StyleSheetList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document_or_shadow_root);
    visitor.visit(m_sheets);
}

Optional<JS::Value> StyleSheetList::item_value(size_t index) const
{
    if (index >= m_sheets.size())
        return {};

    return m_sheets[index].ptr();
}

DOM::Document& StyleSheetList::document()
{
    return m_document_or_shadow_root->document();
}

DOM::Document const& StyleSheetList::document() const
{
    return m_document_or_shadow_root->document();
}

}
