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

#include "config.h"

#include <glib.h>
#include <stdio.h>
#include "vteinternal.hh"
#include "drawing-context.hh"
#include "glib-glue.hh"

#include "image.hh"

namespace vte {

namespace image {

/* Paint the image with provided drawing context */
void
Image::paint(vte::view::DrawingContext& draw,
             int offset_x,
             int offset_y,
             int cell_width,
             int cell_height) const noexcept
{
        if (m_src_width <= 0 || m_src_height <= 0 ||
            m_dst_width <= 0 || m_dst_height <= 0)
                return;

        auto const scale_x = double(cell_width) / double(m_cell_width);
        auto const scale_y = double(cell_height) / double(m_cell_height);

        auto const dst_x = offset_x + m_offset_x * scale_x;
        auto const dst_y = offset_y + m_offset_y * scale_y;
        auto const dst_w = m_dst_width * scale_x;
        auto const dst_h = m_dst_height * scale_y;

        /* Never paint outside the cells the placement covers */
        auto const clip_w = int(m_width_cells) * cell_width;
        auto const clip_h = int(m_height_cells) * cell_height;

#if VTE_GTK == 3
        auto const source = m_surface.get();
#elif VTE_GTK == 4
        auto const source = texture();
#endif
        if (!source)
                return;

        draw.draw_image(source,
                        m_src_x, m_src_y, m_src_width, m_src_height,
                        dst_x, dst_y, dst_w, dst_h,
                        offset_x, offset_y, clip_w, clip_h);
}

#if VTE_GTK == 4

GdkTexture*
Image::texture() const noexcept
{
        if (m_texture)
                return m_texture.get();

        auto const surface = m_surface.get();
        if (!surface ||
            cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE ||
            cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)
                return nullptr;

        cairo_surface_flush(surface);

        auto const width = cairo_image_surface_get_width(surface);
        auto const height = cairo_image_surface_get_height(surface);
        auto const stride = cairo_image_surface_get_stride(surface);
        auto const data = cairo_image_surface_get_data(surface);
        if (width <= 0 || height <= 0 || !data)
                return nullptr;

        /* The bytes reference the surface's data; keep the surface alive
         * for as long as the bytes exist. */
        auto bytes = vte::take_freeable
                (g_bytes_new_with_free_func(data,
                                            size_t(stride) * size_t(height),
                                            GDestroyNotify(cairo_surface_destroy),
                                            cairo_surface_reference(surface)));

        /* CAIRO_FORMAT_ARGB32 is native-endian premultiplied ARGB, which
         * is exactly GDK_MEMORY_DEFAULT. */
        m_texture = vte::glib::take_ref
                (gdk_memory_texture_new(width,
                                        height,
                                        GDK_MEMORY_DEFAULT,
                                        bytes.get(),
                                        stride));

        return m_texture.get();
}

#endif /* VTE_GTK == 4 */

vte::Freeable<cairo_surface_t>
Image::normalise_surface(cairo_surface_t* surface) noexcept
{
        if (!surface)
                return nullptr;

        if (cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE &&
            cairo_image_surface_get_format(surface) == CAIRO_FORMAT_ARGB32)
                return vte::take_freeable(cairo_surface_reference(surface));

        auto const width = cairo_image_surface_get_width(surface);
        auto const height = cairo_image_surface_get_height(surface);
        if (width <= 0 || height <= 0)
                return nullptr;

        auto copy = vte::take_freeable(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
        if (cairo_surface_status(copy.get()) != CAIRO_STATUS_SUCCESS)
                return nullptr;

        auto cr = vte::take_freeable(cairo_create(copy.get()));
        cairo_set_source_surface(cr.get(), surface, 0, 0);
        cairo_set_operator(cr.get(), CAIRO_OPERATOR_SOURCE);
        cairo_paint(cr.get());
        cairo_surface_flush(copy.get());

        return copy;
}

} // namespace image

} // namespace vte
