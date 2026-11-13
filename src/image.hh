/*
 * Copyright © 2016-2020 Hayaki Saito
 * Copyright © 2020 Hans Petter Jansson
 * Copyright © 2026 Vmux contributors
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <algorithm>
#include <cstdint>

#include <pango/pangocairo.h>
#include "cairo-glue.hh"
#include "fwd.hh"

#if VTE_GTK == 4
#include <gtk/gtk.h>
#include "refptr.hh"
#endif

namespace vte {

namespace image {

/* Which protocol placed the image. Affects lifetime rules:
 * SIXEL and ITERM2 images are "painted into" the grid: they are erased
 * by later images that completely cover them, and are accounted against
 * the ring's memory budget.
 * KITTY placements are references to data owned by the kitty image store;
 * they are only ever removed by explicit delete commands, screen clears,
 * or when the rows they anchor to drop out of the ring.
 */
enum class Kind : uint8_t {
        SIXEL,
        KITTY,
        ITERM2,
};

/* Kitty z-index threshold: placements with a z-index below this are drawn
 * underneath cells with a non-default background colour. */
inline constexpr int32_t k_z_below_background = -1073741824; // INT32_MIN / 2

class Image {
private:
        // Image data, device-independent. May be shared between several
        // placements (cairo surfaces are reference counted).
        vte::Freeable<cairo_surface_t> m_surface{};

#if VTE_GTK == 4
        // Lazily created texture for the GSK renderer. Since the surface
        // is immutable once created, the texture never needs invalidating.
        mutable vte::glib::RefPtr<GdkTexture> m_texture{};
#endif

        // Draw/prune priority, must be unique
        size_t m_priority{0};

        Kind m_kind;

        // Kitty graphics protocol identifiers (0 if unset)
        uint32_t m_image_id{0};
        uint32_t m_placement_id{0};
        uint32_t m_image_number{0};

        // Stacking order (kitty z-index; SIXEL and iTerm2 images use 0)
        int32_t m_z{0};

        // Source rectangle inside the surface, in surface pixels
        int m_src_x{0};
        int m_src_y{0};
        int m_src_width{0};
        int m_src_height{0};

        // Size the source rectangle is displayed at, in pixels at the
        // cell size current when the placement was created.
        int m_dst_width{0};
        int m_dst_height{0};

        // Pixel offset of the image's top-left corner inside the top-left
        // cell, at creation cell size.
        int m_offset_x{0};
        int m_offset_y{0};

        // Top left corner offset in cell units
        long m_left_cells{0};
        long m_top_cells{0};

        // Extent in cells
        int m_width_cells{0};
        int m_height_cells{0};

        // Cell dimensions in pixels at time of image creation
        int m_cell_width{1};
        int m_cell_height{1};

        // Whether this image's pixel data is accounted against the ring's
        // memory budget (false for kitty placements, whose data is
        // accounted in the kitty image store).
        bool m_owns_memory{true};

public:
        /* Creates a placement that shows the source rectangle
         * (src_x, src_y, src_width, src_height) of @surface at
         * cell (col, row), offset by (offset_x, offset_y) pixels, scaled to
         * dst_width x dst_height pixels, covering width_cells x height_cells
         * cells. All pixel values are relative to the cell size
         * cell_width x cell_height current at creation time; when painted at
         * a different cell size, everything is scaled proportionally so that
         * the image keeps covering the same cells.
         */
        Image(Kind kind,
              vte::Freeable<cairo_surface_t> surface,
              int src_x,
              int src_y,
              int src_width,
              int src_height,
              int dst_width,
              int dst_height,
              int offset_x,
              int offset_y,
              long col,
              long row,
              int width_cells,
              int height_cells,
              int cell_width,
              int cell_height) noexcept
                : m_surface{std::move(surface)},
                  m_kind{kind},
                  m_src_x{src_x},
                  m_src_y{src_y},
                  m_src_width{src_width},
                  m_src_height{src_height},
                  m_dst_width{dst_width},
                  m_dst_height{dst_height},
                  m_offset_x{offset_x},
                  m_offset_y{offset_y},
                  m_left_cells{col},
                  m_top_cells{row},
                  m_width_cells{width_cells},
                  m_height_cells{height_cells},
                  m_cell_width{cell_width > 0 ? cell_width : 1},
                  m_cell_height{cell_height > 0 ? cell_height : 1}
        {
        }

        /* Creates a SIXEL-style placement: the whole surface shown at its
         * natural size at cell (col, row).
         */
        Image(vte::Freeable<cairo_surface_t> surface,
              int width_pixels,
              int height_pixels,
              long col,
              long row,
              int cell_width,
              int cell_height) noexcept
                : Image(Kind::SIXEL,
                        std::move(surface),
                        0, 0, width_pixels, height_pixels,
                        width_pixels, height_pixels,
                        0, 0,
                        col, row,
                        (width_pixels + std::max(cell_width, 1) - 1) / std::max(cell_width, 1),
                        (height_pixels + std::max(cell_height, 1) - 1) / std::max(cell_height, 1),
                        cell_width, cell_height)
        {
        }

        ~Image() = default;

        Image(Image const&) = delete;
        Image(Image&&) = delete;
        Image operator=(Image const&) = delete;
        Image operator=(Image&&) = delete;

        inline constexpr auto kind() const noexcept { return m_kind; }
        inline constexpr auto get_priority() const noexcept { return m_priority; }
        inline void set_priority(size_t priority) noexcept { m_priority = priority; }

        inline constexpr auto image_id() const noexcept { return m_image_id; }
        inline constexpr auto placement_id() const noexcept { return m_placement_id; }
        inline constexpr auto image_number() const noexcept { return m_image_number; }
        inline void set_ids(uint32_t image_id,
                            uint32_t placement_id,
                            uint32_t image_number) noexcept
        {
                m_image_id = image_id;
                m_placement_id = placement_id;
                m_image_number = image_number;
        }

        inline constexpr auto z() const noexcept { return m_z; }
        inline void set_z(int32_t z) noexcept { m_z = z; }

        inline constexpr auto owns_memory() const noexcept { return m_owns_memory; }
        inline void set_owns_memory(bool owns) noexcept { m_owns_memory = owns; }

        inline auto surface() const noexcept { return m_surface.get(); }

        inline constexpr auto get_left() const noexcept { return m_left_cells; }
        inline auto get_top() const noexcept { return m_top_cells; }
        inline void set_top(long row) noexcept { m_top_cells = row; }
        inline constexpr auto get_width() const noexcept { return m_width_cells; }
        inline constexpr auto get_height() const noexcept { return m_height_cells; }
        inline auto get_bottom() const noexcept { return m_top_cells + get_height() - 1; }
        inline auto get_right() const noexcept { return m_left_cells + get_width() - 1; }

        inline bool intersects_rows(long top,
                                    long bottom) const noexcept
        {
                return get_bottom() >= top && get_top() <= bottom;
        }

        inline bool contains_cell(long col,
                                  long row) const noexcept
        {
                return row >= get_top() && row <= get_bottom() &&
                        col >= get_left() && col <= get_right();
        }

        inline auto resource_size() const noexcept
        {
                if (!m_owns_memory)
                        return 0;

                if (cairo_image_surface_get_stride(m_surface.get()) != 0)
                        return cairo_image_surface_get_stride(m_surface.get()) *
                                cairo_image_surface_get_height(m_surface.get());

                /* Not an image surface: Only the device knows for sure, so we guess */
                return cairo_image_surface_get_width(m_surface.get()) *
                        cairo_image_surface_get_height(m_surface.get()) * 4;
        }

        /* Paints the image with its top-left cell at (offset_x, offset_y)
         * pixels, using the current cell size to scale.
         */
        void paint(vte::view::DrawingContext& draw,
                   int offset_x,
                   int offset_y,
                   int cell_width,
                   int cell_height) const noexcept;

        /* Paints the columns x rows cells starting at (column, row) of the
         * placement's cell grid with the first of them at (offset_x,
         * offset_y) pixels. Used for Unicode placeholders, where each
         * rectangle of placeholder cells shows a piece of the image. */
        void paint_cells(vte::view::DrawingContext& draw,
                         int offset_x,
                         int offset_y,
                         int cell_width,
                         int cell_height,
                         int column,
                         int row,
                         int columns,
                         int rows) const noexcept;

#if VTE_GTK == 4
        GdkTexture* texture() const noexcept;
#endif

        /* Creates a texture-backed copy of a cairo image surface, in the
         * CAIRO_FORMAT_ARGB32 format, so that all images in the ring have
         * a uniform format. Returns nullptr on failure. */
        static vte::Freeable<cairo_surface_t> normalise_surface(cairo_surface_t* surface) noexcept;

}; // class Image

} // namespace image

} // namespace vte
