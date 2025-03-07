/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/Traits.h>
#include <LibWeb/Layout/TableFormattingContext.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/TableBordersPainting.h>

struct CellCoordinates {
    size_t row_index;
    size_t column_index;

    bool operator==(CellCoordinates const& other) const
    {
        return row_index == other.row_index && column_index == other.column_index;
    }
};

namespace AK {
template<>
struct Traits<CellCoordinates> : public GenericTraits<CellCoordinates> {
    static unsigned hash(CellCoordinates const& key) { return pair_int_hash(key.row_index, key.column_index); }
};
}

namespace Web::Painting {

static void collect_cell_boxes_with_collapsed_borders(Vector<PaintableBox const*>& cell_boxes, Layout::Node const& box)
{
    box.for_each_child([&](auto& child) {
        if (child.display().is_table_cell() && child.computed_values().border_collapse() == CSS::BorderCollapse::Collapse) {
            VERIFY(is<Layout::Box>(child) && child.paintable());
            cell_boxes.append(static_cast<Layout::Box const&>(child).paintable_box());
        } else {
            collect_cell_boxes_with_collapsed_borders(cell_boxes, child);
        }
    });
}

enum class EdgeDirection {
    Horizontal,
    Vertical,
};

struct BorderEdgePaintingInfo {
    DevicePixelRect rect;
    PaintableBox::BorderDataWithElementKind border_data_with_element_kind;
    EdgeDirection direction;
    Optional<size_t> row;
    Optional<size_t> column;
};

static Optional<size_t> row_index_for_element_kind(size_t index, Painting::PaintableBox::ConflictingElementKind element_kind)
{
    switch (element_kind) {
    case Painting::PaintableBox::ConflictingElementKind::Cell:
    case Painting::PaintableBox::ConflictingElementKind::Row:
    case Painting::PaintableBox::ConflictingElementKind::RowGroup: {
        return index;
    }
    default:
        return {};
    }
}

static Optional<size_t> column_index_for_element_kind(size_t index, Painting::PaintableBox::ConflictingElementKind element_kind)
{
    switch (element_kind) {
    case Painting::PaintableBox::ConflictingElementKind::Cell:
    case Painting::PaintableBox::ConflictingElementKind::Column:
    case Painting::PaintableBox::ConflictingElementKind::ColumnGroup: {
        return index;
    }
    default:
        return {};
    }
}

static BorderEdgePaintingInfo make_right_cell_edge(
    PaintContext& context,
    CSSPixelRect const& right_cell_rect,
    CSSPixelRect const& cell_rect,
    PaintableBox::BordersDataWithElementKind const& borders_data,
    CellCoordinates const& coordinates)
{
    DevicePixelRect right_border_rect = {
        context.rounded_device_pixels(right_cell_rect.x() - round(borders_data.right.border_data.width / 2)),
        context.rounded_device_pixels(cell_rect.y() - round(borders_data.top.border_data.width / 2)),
        context.rounded_device_pixels(borders_data.right.border_data.width),
        context.rounded_device_pixels(max(cell_rect.height(), right_cell_rect.height()) + round(borders_data.top.border_data.width / 2) + round(borders_data.bottom.border_data.width / 2)),
    };
    return BorderEdgePaintingInfo {
        .rect = right_border_rect,
        .border_data_with_element_kind = borders_data.right,
        .direction = EdgeDirection::Vertical,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.right.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.right.element_kind),
    };
}

static BorderEdgePaintingInfo make_down_cell_edge(
    PaintContext& context,
    CSSPixelRect const& down_cell_rect,
    CSSPixelRect const& cell_rect,
    PaintableBox::BordersDataWithElementKind const& borders_data,
    CellCoordinates const& coordinates)
{
    DevicePixelRect down_border_rect = {
        context.rounded_device_pixels(cell_rect.x() - round(borders_data.left.border_data.width / 2)),
        context.rounded_device_pixels(down_cell_rect.y() - round(borders_data.bottom.border_data.width / 2)),
        context.rounded_device_pixels(max(cell_rect.width(), down_cell_rect.width()) + round(borders_data.left.border_data.width / 2) + round(borders_data.right.border_data.width / 2)),
        context.rounded_device_pixels(borders_data.bottom.border_data.width),
    };
    return BorderEdgePaintingInfo {
        .rect = down_border_rect,
        .border_data_with_element_kind = borders_data.bottom,
        .direction = EdgeDirection::Horizontal,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.bottom.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.bottom.element_kind),
    };
}

static BorderEdgePaintingInfo make_first_row_top_cell_edge(PaintContext& context, CSSPixelRect const& cell_rect, PaintableBox::BordersDataWithElementKind const& borders_data, CellCoordinates const& coordinates)
{
    DevicePixelRect top_border_rect = {
        context.rounded_device_pixels(cell_rect.x() - round(borders_data.left.border_data.width / 2)),
        context.rounded_device_pixels(cell_rect.y() - round(borders_data.top.border_data.width / 2)),
        context.rounded_device_pixels(cell_rect.width()),
        context.rounded_device_pixels(borders_data.top.border_data.width),
    };
    return BorderEdgePaintingInfo {
        .rect = top_border_rect,
        .border_data_with_element_kind = borders_data.top,
        .direction = EdgeDirection::Horizontal,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.top.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.top.element_kind),
    };
}

static BorderEdgePaintingInfo make_last_row_bottom_cell_edge(PaintContext& context, CSSPixelRect const& cell_rect, PaintableBox::BordersDataWithElementKind const& borders_data, CellCoordinates const& coordinates)
{
    DevicePixelRect bottom_border_rect = {
        context.rounded_device_pixels(cell_rect.x() - round(borders_data.left.border_data.width / 2)),
        context.rounded_device_pixels(cell_rect.y() + cell_rect.height() - round(borders_data.bottom.border_data.width / 2)),
        context.rounded_device_pixels(cell_rect.width() + round(borders_data.left.border_data.width / 2) + round(borders_data.right.border_data.width / 2)),
        context.rounded_device_pixels(borders_data.bottom.border_data.width),
    };
    return BorderEdgePaintingInfo {
        .rect = bottom_border_rect,
        .border_data_with_element_kind = borders_data.bottom,
        .direction = EdgeDirection::Horizontal,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.bottom.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.bottom.element_kind),
    };
}

static BorderEdgePaintingInfo make_first_column_left_cell_edge(PaintContext& context, CSSPixelRect const& cell_rect, PaintableBox::BordersDataWithElementKind const& borders_data, CellCoordinates const& coordinates)
{
    DevicePixelRect left_border_rect = {
        context.rounded_device_pixels(cell_rect.x() - round(borders_data.left.border_data.width / 2)),
        context.rounded_device_pixels(cell_rect.y() - round(borders_data.top.border_data.width / 2)),
        context.rounded_device_pixels(borders_data.left.border_data.width),
        context.rounded_device_pixels(cell_rect.height() + round(borders_data.top.border_data.width / 2)),
    };
    return BorderEdgePaintingInfo {
        .rect = left_border_rect,
        .border_data_with_element_kind = borders_data.left,
        .direction = EdgeDirection::Vertical,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.left.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.left.element_kind),
    };
}

static BorderEdgePaintingInfo make_last_column_right_cell_edge(PaintContext& context, CSSPixelRect const& cell_rect, PaintableBox::BordersDataWithElementKind const& borders_data, CellCoordinates const& coordinates)
{
    DevicePixelRect right_border_rect = {
        context.rounded_device_pixels(cell_rect.x() + cell_rect.width() - round(borders_data.right.border_data.width / 2)),
        context.rounded_device_pixels(cell_rect.y() - round(borders_data.top.border_data.width / 2)),
        context.rounded_device_pixels(borders_data.right.border_data.width),
        context.rounded_device_pixels(cell_rect.height() + round(borders_data.top.border_data.width / 2) + round(borders_data.bottom.border_data.width / 2)),
    };
    return BorderEdgePaintingInfo {
        .rect = right_border_rect,
        .border_data_with_element_kind = borders_data.right,
        .direction = EdgeDirection::Vertical,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.right.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.right.element_kind),
    };
}

static void paint_collected_edges(PaintContext& context, Vector<BorderEdgePaintingInfo>& border_edge_painting_info_list)
{
    // This sorting step isn't part of the specification, but it matches the behavior of other browsers at border intersections, which aren't
    // part of border conflict resolution in the specification but it's still desirable to handle them in a way which is consistent with it.
    // See https://www.w3.org/TR/CSS22/tables.html#border-conflict-resolution for reference.
    quick_sort(border_edge_painting_info_list, [](auto const& a, auto const& b) {
        auto const& a_border_data = a.border_data_with_element_kind.border_data;
        auto const& b_border_data = b.border_data_with_element_kind.border_data;
        if (a_border_data.line_style == b_border_data.line_style && a_border_data.width == b_border_data.width) {
            if (b.border_data_with_element_kind.element_kind < a.border_data_with_element_kind.element_kind) {
                return true;
            } else if (b.border_data_with_element_kind.element_kind > a.border_data_with_element_kind.element_kind) {
                return false;
            }
            // Here the element kind is the same, thus the coordinates are either both set or not set.
            VERIFY(a.column.has_value() == b.column.has_value());
            VERIFY(a.row.has_value() == b.row.has_value());
            if (a.column.has_value()) {
                if (b.column.value() < a.column.value()) {
                    return true;
                } else if (b.column.value() > a.column.value()) {
                    return false;
                }
            }
            return a.row.has_value() ? b.row.value() < a.row.value() : false;
        }
        return Layout::TableFormattingContext::border_is_less_specific(a_border_data, b_border_data);
    });

    for (auto const& border_edge_painting_info : border_edge_painting_info_list) {
        auto const& border_data_with_element_kind = border_edge_painting_info.border_data_with_element_kind;
        CSSPixels width = border_data_with_element_kind.border_data.width;
        if (width <= 0)
            continue;
        auto color = border_data_with_element_kind.border_data.color;
        auto border_style = border_data_with_element_kind.border_data.line_style;
        auto p1 = border_edge_painting_info.rect.top_left();
        auto p2 = border_edge_painting_info.direction == EdgeDirection::Horizontal
            ? border_edge_painting_info.rect.top_right()
            : border_edge_painting_info.rect.bottom_left();

        if (border_style == CSS::LineStyle::Dotted) {
            Gfx::AntiAliasingPainter aa_painter { context.painter() };
            aa_painter.draw_line(p1.to_type<int>(), p2.to_type<int>(), color, width.to_double(), Gfx::Painter::LineStyle::Dotted);
        } else if (border_style == CSS::LineStyle::Dashed) {
            context.painter().draw_line(p1.to_type<int>(), p2.to_type<int>(), color, width.to_double(), Gfx::Painter::LineStyle::Dashed);
        } else {
            // FIXME: Support the remaining line styles instead of rendering them as solid.
            context.painter().fill_rect(Gfx::IntRect(border_edge_painting_info.rect.location(), border_edge_painting_info.rect.size()), color);
        }
    }
}

void paint_table_collapsed_borders(PaintContext& context, Layout::Node const& box)
{
    // Partial implementation of painting according to the collapsing border model:
    // https://www.w3.org/TR/CSS22/tables.html#collapsing-borders
    Vector<PaintableBox const*> cell_boxes;
    collect_cell_boxes_with_collapsed_borders(cell_boxes, box);
    Vector<BorderEdgePaintingInfo> border_edge_painting_info_list;
    HashMap<CellCoordinates, PaintableBox const*> cell_coordinates_to_box;
    size_t row_count = 0;
    size_t column_count = 0;
    for (auto const cell_box : cell_boxes) {
        cell_coordinates_to_box.set(CellCoordinates {
                                        .row_index = cell_box->table_cell_coordinates()->row_index,
                                        .column_index = cell_box->table_cell_coordinates()->column_index },
            cell_box);
        row_count = max(row_count, cell_box->table_cell_coordinates()->row_index + cell_box->table_cell_coordinates()->row_span);
        column_count = max(column_count, cell_box->table_cell_coordinates()->column_index + cell_box->table_cell_coordinates()->column_span);
    }
    for (auto const cell_box : cell_boxes) {
        auto borders_data = cell_box->override_borders_data().has_value() ? cell_box->override_borders_data().value() : PaintableBox::BordersDataWithElementKind {
            .top = { .border_data = cell_box->box_model().border.top == 0 ? CSS::BorderData() : cell_box->computed_values().border_top(), .element_kind = PaintableBox::ConflictingElementKind::Cell },
            .right = { .border_data = cell_box->box_model().border.right == 0 ? CSS::BorderData() : cell_box->computed_values().border_right(), .element_kind = PaintableBox::ConflictingElementKind::Cell },
            .bottom = { .border_data = cell_box->box_model().border.bottom == 0 ? CSS::BorderData() : cell_box->computed_values().border_bottom(), .element_kind = PaintableBox::ConflictingElementKind::Cell },
            .left = { .border_data = cell_box->box_model().border.left == 0 ? CSS::BorderData() : cell_box->computed_values().border_left(), .element_kind = PaintableBox::ConflictingElementKind::Cell },
        };
        auto cell_rect = cell_box->absolute_border_box_rect();
        CellCoordinates right_cell_coordinates {
            .row_index = cell_box->table_cell_coordinates()->row_index,
            .column_index = cell_box->table_cell_coordinates()->column_index + cell_box->table_cell_coordinates()->column_span
        };
        auto maybe_right_cell = cell_coordinates_to_box.get(right_cell_coordinates);
        CellCoordinates down_cell_coordinates {
            .row_index = cell_box->table_cell_coordinates()->row_index + cell_box->table_cell_coordinates()->row_span,
            .column_index = cell_box->table_cell_coordinates()->column_index
        };
        auto maybe_down_cell = cell_coordinates_to_box.get(down_cell_coordinates);
        if (maybe_right_cell.has_value())
            border_edge_painting_info_list.append(make_right_cell_edge(context, maybe_right_cell.value()->absolute_border_box_rect(), cell_rect, borders_data, right_cell_coordinates));
        if (maybe_down_cell.has_value())
            border_edge_painting_info_list.append(make_down_cell_edge(context, maybe_down_cell.value()->absolute_border_box_rect(), cell_rect, borders_data, down_cell_coordinates));
        if (cell_box->table_cell_coordinates()->row_index == 0)
            border_edge_painting_info_list.append(make_first_row_top_cell_edge(context, cell_rect, borders_data,
                { .row_index = 0, .column_index = cell_box->table_cell_coordinates()->column_index }));
        if (cell_box->table_cell_coordinates()->row_index + cell_box->table_cell_coordinates()->row_span == row_count)
            border_edge_painting_info_list.append(make_last_row_bottom_cell_edge(context, cell_rect, borders_data,
                { .row_index = row_count - 1, .column_index = cell_box->table_cell_coordinates()->column_index }));
        if (cell_box->table_cell_coordinates()->column_index == 0)
            border_edge_painting_info_list.append(make_first_column_left_cell_edge(context, cell_rect, borders_data,
                { .row_index = cell_box->table_cell_coordinates()->row_index, .column_index = 0 }));
        if (cell_box->table_cell_coordinates()->column_index + cell_box->table_cell_coordinates()->column_span == column_count)
            border_edge_painting_info_list.append(make_last_column_right_cell_edge(context, cell_rect, borders_data,
                { .row_index = cell_box->table_cell_coordinates()->row_index, .column_index = column_count - 1 }));
    }

    paint_collected_edges(context, border_edge_painting_info_list);

    for (auto const cell_box : cell_boxes) {
        auto const& border_radii_data = cell_box->normalized_border_radii_data();
        auto top_left = border_radii_data.top_left.as_corner(context);
        auto top_right = border_radii_data.top_right.as_corner(context);
        auto bottom_right = border_radii_data.bottom_right.as_corner(context);
        auto bottom_left = border_radii_data.bottom_left.as_corner(context);
        if (!top_left && !top_right && !bottom_left && !bottom_right) {
            continue;
        } else {
            auto borders_data = cell_box->override_borders_data().has_value() ? PaintableBox::remove_element_kind_from_borders_data(cell_box->override_borders_data().value()) : BordersData {
                .top = cell_box->box_model().border.top == 0 ? CSS::BorderData() : cell_box->computed_values().border_top(),
                .right = cell_box->box_model().border.right == 0 ? CSS::BorderData() : cell_box->computed_values().border_right(),
                .bottom = cell_box->box_model().border.bottom == 0 ? CSS::BorderData() : cell_box->computed_values().border_bottom(),
                .left = cell_box->box_model().border.left == 0 ? CSS::BorderData() : cell_box->computed_values().border_left(),
            };
            paint_all_borders(context, cell_box->absolute_border_box_rect(), cell_box->normalized_border_radii_data(), borders_data);
        }
    }
}

}
