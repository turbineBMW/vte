/*
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

/*
 * Terminal side of the kitty graphics protocol: placing images on the
 * grid, moving the cursor, deleting placements, and replying to the client.
 * See kitty-graphics.hh for the protocol-level parts.
 */

#include "config.h"

#include <algorithm>
#include <cmath>

#include "vteinternal.hh"
#include "kitty-graphics.hh"
#include "image.hh"

using namespace std::literals;

namespace vte {
namespace terminal {

vte::kitty::Store&
Terminal::kitty_store()
{
        if (!m_kitty_store)
                m_kitty_store = std::make_unique<vte::kitty::Store>();
        return *m_kitty_store;
}

/* Sends a response for @cmd, if one is wanted. @image_id overrides the
 * command's image id (used when the terminal allocated the id). */
void
Terminal::kitty_reply(vte::parser::Sequence const& seq,
                      vte::kitty::Command const& cmd,
                      std::string_view message,
                      uint32_t image_id)
{
        if (!cmd.wants_response(message == "OK"sv))
                return;

        auto str = fmt::format("Gi={}", image_id ? image_id : cmd.image_id);
        if (cmd.has_image_number)
                fmt::format_to(std::back_inserter(str), ",I={}", cmd.image_number);
        if (cmd.has_placement_id)
                fmt::format_to(std::back_inserter(str), ",p={}", cmd.placement_id);
        str.push_back(';');
        str.append(message);

        auto builder = vte::parser::u8SequenceBuilder{VTE_SEQ_APC};
        builder.set_string(std::move(str));
        reply(seq, builder);
}

bool
Terminal::kitty_image_has_placements(uint32_t image_id) const noexcept
{
        auto const it = m_kitty_virtual_placements.lower_bound({image_id, 0});
        if (it != m_kitty_virtual_placements.end() && it->first.first == image_id)
                return true;

        return m_normal_screen.row_data->has_image_with_id(image_id) ||
                m_alternate_screen.row_data->has_image_with_id(image_id);
}

/* Removes the virtual placements matching @pred. Returns how many. */
size_t
Terminal::kitty_remove_virtual_placements_if(std::function<bool(vte::image::Image const&)> const& pred)
{
        auto const removed = std::erase_if(m_kitty_virtual_placements, [&](auto const& item) {
                return pred(*item.second.image);
        });
        if (removed)
                invalidate_all();
        return removed;
}

/* The virtual placement placeholder cells for (@image_id, @placement_id)
 * show. Without a placement id, or if there is no such placement, that is
 * the image's most recently created virtual placement. */
vte::image::Image const*
Terminal::kitty_find_virtual_placement(uint32_t image_id,
                                       uint32_t placement_id) const noexcept
{
        if (placement_id != 0) {
                auto const it = m_kitty_virtual_placements.find({image_id, placement_id});
                if (it != m_kitty_virtual_placements.end())
                        return it->second.image.get();
        }

        auto const* latest = static_cast<KittyVirtualPlacement const*>(nullptr);
        for (auto it = m_kitty_virtual_placements.lower_bound({image_id, 0});
             it != m_kitty_virtual_placements.end() && it->first.first == image_id;
             ++it) {
                if (!latest || it->second.sequence > latest->sequence)
                        latest = &it->second;
        }
        return latest ? latest->image.get() : nullptr;
}

void
Terminal::kitty_delete_placements_for_image(uint32_t image_id)
{
        auto removed = size_t{0};
        for (auto* screen : {&m_normal_screen, &m_alternate_screen}) {
                removed += screen->row_data->remove_images_if([image_id](auto const& image) noexcept {
                        return image.kind() == vte::image::Kind::KITTY &&
                                image.image_id() == image_id;
                });
        }
        if (removed)
                invalidate_all();

        kitty_remove_virtual_placements_if([image_id](auto const& image) noexcept {
                return image.image_id() == image_id;
        });
}

/* Called from clear_screen(): "The clear screen escape code should also
 * clear all images". Removes the kitty placements intersecting the screen
 * area of the current screen. Other protocols keep their images, which
 * scroll into the scrollback together with the text. */
void
Terminal::kitty_clear_screen()
{
        auto const top = m_screen->insert_delta;
        auto const bottom = top + m_row_count - 1;
        auto const removed = m_screen->row_data->remove_images_if([top, bottom](auto const& image) noexcept {
                return image.kind() == vte::image::Kind::KITTY &&
                        image.intersects_rows(top, bottom);
        });
        if (removed)
                invalidate_all();
}

/* Removes all kitty placements from @screen (used when leaving the
 * alternate screen, and on reset). */
void
Terminal::kitty_clear_placements(VteScreen* screen)
{
        auto const removed = screen->row_data->remove_images_if([](auto const& image) noexcept {
                return image.kind() == vte::image::Kind::KITTY;
        });
        if (removed)
                invalidate_all();
}

void
Terminal::kitty_reset()
{
        kitty_clear_placements(&m_normal_screen);
        kitty_clear_placements(&m_alternate_screen);
        if (!m_kitty_virtual_placements.empty()) {
                m_kitty_virtual_placements.clear();
                invalidate_all();
        }
        if (m_kitty_store)
                m_kitty_store->clear();
}

/* Entry point: @str is the control data and payload after the 'G'. */
void
Terminal::kitty_graphics_command(vte::parser::Sequence const& seq,
                                 std::u32string_view str)
{
        if (!m_kitty_enabled)
                return;

        /* Control data and payload are ASCII */
        auto s = std::string{};
        s.reserve(str.size());
        for (auto const c : str) {
                if (c > 0x7f)
                        return;
                s.push_back(char(c));
        }

        auto const semicolon = s.find(';');
        auto const control = std::string_view{s}.substr(0, semicolon);
        auto const payload = semicolon == std::string::npos
                ? std::string_view{}
                : std::string_view{s}.substr(semicolon + 1);

        auto& store = kitty_store();
        auto& upload = store.upload();

        auto cmd = vte::kitty::Command{};
        auto error = std::string{};
        if (!cmd.parse(control, error)) {
                upload.reset();
                kitty_reply(seq, cmd, error);
                return;
        }

        /* A chunked transmission in progress swallows everything except
         * delete commands, which abort it. */
        if (upload.active) {
                if (cmd.action == 'd') {
                        upload.reset();
                } else {
                        auto chunk = std::string{};
                        if (!vte::kitty::base64_decode(payload, chunk)) {
                                auto const command = upload.command;
                                upload.reset();
                                kitty_reply(seq, command, "EINVAL:Malformed base64 payload");
                                return;
                        }

                        if (upload.data.size() + chunk.size() > vte::kitty::k_max_file_size) {
                                auto const command = upload.command;
                                upload.reset();
                                kitty_reply(seq, command, "EFBIG:Image data too large");
                                return;
                        }

                        upload.data.append(chunk);
                        if (cmd.more)
                                return;

                        auto command = upload.command;
                        if (cmd.quiet)
                                command.quiet = cmd.quiet;
                        auto data = std::move(upload.data);
                        upload.reset();
                        kitty_transmit(seq, command, std::move(data));
                        return;
                }
        }

        switch (cmd.action) {
        case 'q':
        case 't':
        case 'T': {
                auto decoded = std::string{};
                if (!vte::kitty::base64_decode(payload, decoded)) {
                        kitty_reply(seq, cmd, "EINVAL:Malformed base64 payload");
                        return;
                }

                if (cmd.medium == 'd' && cmd.more) {
                        upload.command = cmd;
                        upload.data = std::move(decoded);
                        upload.active = true;
                        return;
                }

                auto data = std::string{};
                if (!vte::kitty::load_data(cmd, std::move(decoded), data, error)) {
                        kitty_reply(seq, cmd, error);
                        return;
                }

                kitty_transmit(seq, cmd, std::move(data));
                return;
        }

        case 'p':
                kitty_put(seq, cmd);
                return;

        case 'd':
                kitty_delete(cmd);
                return;

        case 'f':
        case 'a':
        case 'c':
                kitty_reply(seq, cmd, "ENOTSUP:Animation is not supported");
                return;

        default:
                kitty_reply(seq, cmd, "EINVAL:Unknown action");
                return;
        }
}

/* Decodes @data and, depending on the action, stores and/or displays it. */
void
Terminal::kitty_transmit(vte::parser::Sequence const& seq,
                         vte::kitty::Command const& cmd,
                         std::string data)
{
        auto bitmap = vte::kitty::Bitmap{};
        auto error = std::string{};
        if (!vte::kitty::decode_image(cmd, data, bitmap, error)) {
                kitty_reply(seq, cmd, error);
                return;
        }
        data.clear();
        data.shrink_to_fit();

        if (cmd.action == 'q') {
                kitty_reply(seq, cmd, "OK");
                return;
        }

        auto& store = kitty_store();

        auto image_id = cmd.image_id;
        if (cmd.has_image_number) {
                image_id = store.allocate_id();
                if (image_id == 0) {
                        kitty_reply(seq, cmd, "ENOSPC:No free image ids");
                        return;
                }
        }

        /* An image without an id can only be displayed right now; there
         * is no way to refer to it later, so it is not stored. */
        if (image_id == 0) {
                if (cmd.action == 'T')
                        kitty_place(seq, cmd, bitmap.surface.get(), bitmap.width, bitmap.height, 0, 0);
                return;
        }

        /* Make room within the quota */
        auto evicted = std::vector<uint32_t>{};
        if (!store.evict_for(bitmap.bytes(),
                             [this](uint32_t id) { return kitty_image_has_placements(id); },
                             evicted)) {
                kitty_reply(seq, cmd, "ENOSPC:Image storage quota exceeded");
                return;
        }
        for (auto const id : evicted) {
                kitty_delete_placements_for_image(id);
                store.remove(id);
        }

        /* Re-transmitting an id replaces the image and drops its placements */
        if (store.find(image_id))
                kitty_delete_placements_for_image(image_id);

        auto const width = bitmap.width;
        auto const height = bitmap.height;
        auto const* image = store.add(image_id, cmd.image_number, std::move(bitmap), (cmd.hints & 1) != 0);
        if (!image) {
                kitty_reply(seq, cmd, "ENOMEM:Failed to store image");
                return;
        }

        if (cmd.action == 'T')
                kitty_place(seq, cmd, image->surface(), width, height, image_id, cmd.image_number);
        else
                kitty_reply(seq, cmd, "OK", image_id);
}

void
Terminal::kitty_put(vte::parser::Sequence const& seq,
                    vte::kitty::Command const& cmd)
{
        auto& store = kitty_store();

        auto const* image = cmd.has_image_number
                ? store.find_by_number(cmd.image_number)
                : store.find(cmd.image_id);
        if (!image) {
                kitty_reply(seq, cmd, "ENOENT:Image not found");
                return;
        }

        kitty_place(seq, cmd, image->surface(), image->width(), image->height(),
                    image->id(), cmd.image_number);
}

/* Creates a placement of the given surface at the cursor position, moves
 * the cursor, and replies. */
void
Terminal::kitty_place(vte::parser::Sequence const& seq,
                      vte::kitty::Command const& cmd,
                      cairo_surface_t* surface,
                      int image_width,
                      int image_height,
                      uint32_t image_id,
                      uint32_t image_number)
{
        auto const cell_width = int(m_cell_width_unscaled);
        auto const cell_height = int(m_cell_height_unscaled);
        if (!surface || image_width <= 0 || image_height <= 0 ||
            cell_width <= 0 || cell_height <= 0) {
                kitty_reply(seq, cmd, "EINVAL:Cannot place image", image_id);
                return;
        }

        /* Source rectangle, clipped to the image */
        if (cmd.src_x >= uint32_t(image_width) || cmd.src_y >= uint32_t(image_height)) {
                kitty_reply(seq, cmd, "EINVAL:Source rectangle outside the image", image_id);
                return;
        }
        auto const src_x = int(cmd.src_x);
        auto const src_y = int(cmd.src_y);
        auto const src_width = cmd.src_width
                ? std::min(int(std::min<uint32_t>(cmd.src_width, G_MAXINT)), image_width - src_x)
                : image_width - src_x;
        auto const src_height = cmd.src_height
                ? std::min(int(std::min<uint32_t>(cmd.src_height, G_MAXINT)), image_height - src_y)
                : image_height - src_y;

        /* Offset within the first cell must be smaller than the cell */
        auto const offset_x = int(std::min<uint32_t>(cmd.cell_x_offset, cell_width - 1));
        auto const offset_y = int(std::min<uint32_t>(cmd.cell_y_offset, cell_height - 1));

        /* Destination size in pixels and cells */
        auto const max_cells = uint32_t{4096};
        auto const req_columns = std::min(cmd.columns, max_cells);
        auto const req_rows = std::min(cmd.rows, max_cells);

        auto columns = 0;
        auto rows = 0;
        auto dst_width = 0.0;
        auto dst_height = 0.0;
        if (req_columns && req_rows) {
                columns = int(req_columns);
                rows = int(req_rows);
                /* Letterbox / pillarbox into the requested cell rectangle */
                auto const scale = std::min(double(columns * cell_width) / src_width,
                                            double(rows * cell_height) / src_height);
                dst_width = src_width * scale;
                dst_height = src_height * scale;
        } else if (req_columns) {
                columns = int(req_columns);
                dst_width = double(columns * cell_width);
                dst_height = dst_width * src_height / src_width;
                rows = int(std::ceil((dst_height + offset_y) / cell_height));
        } else if (req_rows) {
                rows = int(req_rows);
                dst_height = double(rows * cell_height);
                dst_width = dst_height * src_width / src_height;
                columns = int(std::ceil((dst_width + offset_x) / cell_width));
        } else {
                dst_width = src_width;
                dst_height = src_height;
                columns = (src_width + offset_x + cell_width - 1) / cell_width;
                rows = (src_height + offset_y + cell_height - 1) / cell_height;
        }
        columns = std::clamp(columns, 1, int(max_cells));
        rows = std::clamp(rows, 1, int(max_cells));

        if (cmd.unicode_placeholder) {
                kitty_place_virtual(seq, cmd, surface,
                                    src_x, src_y, src_width, src_height,
                                    columns, rows,
                                    image_id, image_number);
                return;
        }

        place_inline_image(vte::image::Kind::KITTY,
                           surface,
                           src_x, src_y, src_width, src_height,
                           int(std::lround(dst_width)), int(std::lround(dst_height)),
                           offset_x, offset_y,
                           columns, rows,
                           image_id, cmd.placement_id, image_number,
                           cmd.z,
                           false /* data is owned by the store */,
                           cmd.cursor_policy == 0);

        kitty_reply(seq, cmd, "OK", image_id);
}

/* Creates a virtual placement, shown wherever placeholder cells refer to
 * it. The image is fitted into @columns x @rows cells, keeping its aspect
 * ratio, and centred. The cursor does not move. */
void
Terminal::kitty_place_virtual(vte::parser::Sequence const& seq,
                              vte::kitty::Command const& cmd,
                              cairo_surface_t* surface,
                              int src_x,
                              int src_y,
                              int src_width,
                              int src_height,
                              int columns,
                              int rows,
                              uint32_t image_id,
                              uint32_t image_number)
{
        /* Placeholder cells can only refer to an image by its id */
        if (image_id == 0) {
                kitty_reply(seq, cmd, "EINVAL:Virtual placement needs an image id", image_id);
                return;
        }

        auto const cell_width = int(m_cell_width_unscaled);
        auto const cell_height = int(m_cell_height_unscaled);
        auto const box_width = double(columns) * cell_width;
        auto const box_height = double(rows) * cell_height;
        auto const scale = std::min(box_width / src_width, box_height / src_height);
        auto const dst_width = std::max(1, int(std::lround(src_width * scale)));
        auto const dst_height = std::max(1, int(std::lround(src_height * scale)));
        auto const offset_x = int(std::lround((box_width - dst_width) / 2));
        auto const offset_y = int(std::lround((box_height - dst_height) / 2));

        auto image = std::make_unique<vte::image::Image>(vte::image::Kind::KITTY,
                                                         vte::take_freeable(cairo_surface_reference(surface)),
                                                         src_x, src_y, src_width, src_height,
                                                         dst_width, dst_height,
                                                         offset_x, offset_y,
                                                         0, 0,
                                                         columns, rows,
                                                         cell_width, cell_height);
        image->set_ids(image_id, cmd.placement_id, image_number);
        image->set_z(cmd.z);
        image->set_owns_memory(false);

        /* Same image and placement id replaces the old one */
        auto& slot = m_kitty_virtual_placements[{image_id, cmd.placement_id}];
        slot.image = std::move(image);
        slot.sequence = ++m_kitty_virtual_sequence;

        /* Placeholder cells may already be on screen */
        invalidate_all();

        kitty_reply(seq, cmd, "OK", image_id);
}

void
Terminal::kitty_delete(vte::kitty::Command const& cmd)
{
        auto& store = kitty_store();
        auto* ring = m_screen->row_data;

        auto const what = cmd.delete_what;
        auto const free_data = g_ascii_isupper(what);
        auto const which = g_ascii_tolower(what);

        auto const screen_top = m_screen->insert_delta;
        auto const screen_bottom = screen_top + m_row_count - 1;

        /* Resolve the image id an id-based delete refers to */
        auto target_id = uint32_t{0};
        if (which == 'i' || which == 'n') {
                if (cmd.has_image_number || which == 'n') {
                        auto const* image = store.find_by_number(cmd.image_number);
                        if (!image)
                                return;
                        target_id = image->id();
                } else {
                        target_id = cmd.image_id;
                }
        }

        auto pred = std::function<bool(vte::image::Image const&)>{};
        switch (which) {
        case 'a':
                pred = [=](auto const& image) noexcept {
                        return image.intersects_rows(screen_top, screen_bottom);
                };
                break;

        case 'i':
        case 'n': {
                auto const placement_id = cmd.has_placement_id ? cmd.placement_id : 0;
                pred = [=](auto const& image) noexcept {
                        return image.image_id() == target_id &&
                                (placement_id == 0 || image.placement_id() == placement_id);
                };
                break;
        }

        case 'c': {
                auto const col = m_screen->cursor.col;
                auto const row = m_screen->cursor.row;
                pred = [=](auto const& image) noexcept {
                        return image.contains_cell(col, row);
                };
                break;
        }

        case 'p':
        case 'q': {
                auto const col = long(cmd.src_x) - 1;
                auto const row = screen_top + long(cmd.src_y) - 1;
                auto const check_z = which == 'q';
                auto const z = cmd.z;
                pred = [=](auto const& image) noexcept {
                        return image.contains_cell(col, row) &&
                                (!check_z || image.z() == z);
                };
                break;
        }

        case 'r': {
                auto const lo = cmd.src_x;
                auto const hi = cmd.src_y;
                pred = [=](auto const& image) noexcept {
                        return image.image_id() >= lo && image.image_id() <= hi;
                };
                break;
        }

        case 'x': {
                auto const col = long(cmd.src_x) - 1;
                pred = [=](auto const& image) noexcept {
                        return col >= image.get_left() && col <= image.get_right();
                };
                break;
        }

        case 'y': {
                auto const row = screen_top + long(cmd.src_y) - 1;
                pred = [=](auto const& image) noexcept {
                        return row >= image.get_top() && row <= image.get_bottom();
                };
                break;
        }

        case 'z': {
                auto const z = cmd.z;
                pred = [=](auto const& image) noexcept {
                        return image.z() == z;
                };
                break;
        }

        case 'f': /* animation frames: unsupported */
        default:
                return;
        }

        auto affected = std::vector<uint32_t>{};
        auto const removed = ring->remove_images_if([&](auto const& image) {
                if (image.kind() != vte::image::Kind::KITTY || !pred(image))
                        return false;
                if (free_data && image.image_id() != 0)
                        affected.push_back(image.image_id());
                return true;
        });

        /* Virtual placements have no position on screen, so like kitty
         * only the id-based deletes remove them. */
        switch (which) {
        case 'i':
        case 'n':
        case 'r':
                kitty_remove_virtual_placements_if([&](auto const& image) {
                        if (!pred(image))
                                return false;
                        if (free_data)
                                affected.push_back(image.image_id());
                        return true;
                });
                break;
        default:
                break;
        }

        if (free_data) {
                /* Id-based deletes also free images that had no placements */
                if (which == 'i' || which == 'n') {
                        affected.push_back(target_id);
                } else if (which == 'r') {
                        for (auto const& [id, image] : store.images())
                                if (id >= cmd.src_x && id <= cmd.src_y)
                                        affected.push_back(id);
                }

                std::sort(affected.begin(), affected.end());
                affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
                for (auto const id : affected)
                        if (!kitty_image_has_placements(id))
                                store.remove(id);
        }

        if (removed)
                invalidate_all();
}

} // namespace terminal
} // namespace vte
