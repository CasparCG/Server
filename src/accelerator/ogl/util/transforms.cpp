
#include "transforms.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace caspar::accelerator::ogl {

draw_crop_region::draw_crop_region(double left, double top, double right, double bottom)
{
    // upper left
    coords[0]    = t_point(3);
    coords[0](0) = left;
    coords[0](1) = top;
    coords[0](2) = 1;

    // upper right
    coords[1]    = t_point(3);
    coords[1](0) = right;
    coords[1](1) = top;
    coords[1](2) = 1;

    // lower right
    coords[2]    = t_point(3);
    coords[2](0) = right;
    coords[2](1) = bottom;
    coords[2](2) = 1;

    // lower left
    coords[3]    = t_point(3);
    coords[3](0) = left;
    coords[3](1) = bottom;
    coords[3](2) = 1;
}

void draw_crop_region::apply_transform(const caspar::accelerator::ogl::t_matrix& matrix)
{
    coords[0] = coords[0] * matrix;
    coords[1] = coords[1] * matrix;
    coords[2] = coords[2] * matrix;
    coords[3] = coords[3] * matrix;
}

void apply_transform_colour_values(core::image_transform& self, const core::image_transform& other)
{
    // Note: this intentionally does not affect any geometry-related fields, they follow a separate flow

    self.opacity *= other.opacity;
    self.brightness *= other.brightness;
    self.contrast *= other.contrast;
    self.saturation *= other.saturation;

    self.levels.min_input  = std::max(self.levels.min_input, other.levels.min_input);
    self.levels.max_input  = std::min(self.levels.max_input, other.levels.max_input);
    self.levels.min_output = std::max(self.levels.min_output, other.levels.min_output);
    self.levels.max_output = std::min(self.levels.max_output, other.levels.max_output);
    self.levels.gamma *= other.levels.gamma;
    self.chroma.enable |= other.chroma.enable;
    self.chroma.show_mask |= other.chroma.show_mask;
    self.chroma.target_hue     = std::max(other.chroma.target_hue, self.chroma.target_hue);
    self.chroma.min_saturation = std::max(other.chroma.min_saturation, self.chroma.min_saturation);
    self.chroma.min_brightness = std::max(other.chroma.min_brightness, self.chroma.min_brightness);
    self.chroma.hue_width      = std::max(other.chroma.hue_width, self.chroma.hue_width);
    self.chroma.softness       = std::max(other.chroma.softness, self.chroma.softness);
    self.chroma.spill_suppress = std::max(other.chroma.spill_suppress, self.chroma.spill_suppress);
    self.chroma.spill_suppress_saturation =
        std::min(other.chroma.spill_suppress_saturation, self.chroma.spill_suppress_saturation);
    self.is_key |= other.is_key;
    self.invert |= other.invert;
    self.is_mix |= other.is_mix;
    self.blend_mode = std::max(self.blend_mode, other.blend_mode);
    self.layer_depth += other.layer_depth;

    // Gamut compression
    if (other.gamut_compress) {
        self.gamut_compress = true;
        self.gc_cyan        = other.gc_cyan;
        self.gc_magenta     = other.gc_magenta;
        self.gc_yellow      = other.gc_yellow;
    }

    // Secondary qualifier
    if (other.qualifier_enable) {
        self.qualifier_enable = true;
        self.qual_target_hue  = other.qual_target_hue;
        self.qual_hue_width   = other.qual_hue_width;
        self.qual_min_sat     = other.qual_min_sat;
        self.qual_max_sat     = other.qual_max_sat;
        self.qual_min_lum     = other.qual_min_lum;
        self.qual_max_lum     = other.qual_max_lum;
        self.qual_softness    = other.qual_softness;
        self.qual_exposure    = other.qual_exposure;
        self.qual_sat_offset  = other.qual_sat_offset;
        self.qual_hue_offset  = other.qual_hue_offset;
    }

    // Per-channel RGB levels: intersect input range, multiply gamma, intersect output range
    if (other.per_channel_levels.enable) {
        self.per_channel_levels.enable = true;
        auto merge_ch = [](core::rgb_levels_channel& s, const core::rgb_levels_channel& o) {
            s.min_input  = std::max(s.min_input, o.min_input);
            s.max_input  = std::min(s.max_input, o.max_input);
            s.gamma     *= o.gamma;
            s.min_output = std::max(s.min_output, o.min_output);
            s.max_output = std::min(s.max_output, o.max_output);
        };
        merge_ch(self.per_channel_levels.r, other.per_channel_levels.r);
        merge_ch(self.per_channel_levels.g, other.per_channel_levels.g);
        merge_ch(self.per_channel_levels.b, other.per_channel_levels.b);
    }

    // Every combined grading value below is clamped back into the range its MIXER
    // command accepts (core::grade_limits, the same table the commands validate
    // against). Two layers at the edge of legal would otherwise reach a value no single
    // command could set: lift 0.9 over lift 0.9 is 1.8, and a midtone of 0.2 under
    // another 0.2 is an exponent of 25. Clamping here rather than in the shader keeps
    // the value the kernel reports and the value it renders the same thing.
    //
    // The bounds are wide enough that ordinary stacking is untouched -- gain 2 over
    // gain 2 is 4, well inside [0, 10]. hue_shift is the exception and is wrapped
    // instead, below, because rotation is periodic.
    namespace lim = core::grade_limits;

    // White balance: additive
    self.temperature = lim::temperature.clamp(self.temperature + other.temperature);
    self.tint        = lim::tint.clamp(self.tint + other.tint);

    // Lift/Midtone/Gain: additive lift, multiplicative midtone+gain
    for (int i = 0; i < 3; ++i) {
        self.lift[i]    = lim::lift.clamp(self.lift[i] + other.lift[i]);
        self.midtone[i] = lim::midtone.clamp(self.midtone[i] * other.midtone[i]);
        self.gain[i]    = lim::gain.clamp(self.gain[i] * other.gain[i]);
    }

    // Hue shift: additive, then wrapped into [-180, 180].
    //
    // Wrapped, not clamped: 200 degrees of rotation is -160, not 180. The shader rotates
    // with fract(), so rotation is already periodic and this does not change what is
    // rendered -- what it does is bound the accumulation. Stacking layer, channel and
    // tweened transforms can otherwise walk hue_shift arbitrarily far from zero, which
    // costs precision in fract() and makes the "is this effect active" test dishonest:
    // an accumulated 360 is exactly identity but reads as active and pays for the branch.
    self.hue_shift = std::remainder(self.hue_shift + other.hue_shift, 360.0);

    // Tone balance: additive
    self.shadows    = lim::tone.clamp(self.shadows + other.shadows);
    self.highlights = lim::tone.clamp(self.highlights + other.highlights);

    // Split toning: colours add, and the balance of whichever transform actually has
    // split toning active wins.
    //
    // split_balance is a crossover position in luma, not a strength, so no arithmetic
    // composition of two of them means anything: adding 0.5 and 0.5 gives 1.0 (everything
    // is shadow), multiplying gives 0.25. And the shader has one crossover and one colour
    // pair, so two stacked split tones cannot both be represented however they are
    // combined. Summing the colours and keeping a single crossover is the least-lossy
    // approximation available. "Is other active" is the same test the mixer uses to decide
    // whether to enable the effect at all; comparing other.split_balance against its 0.5
    // default instead would misread a tweened 0.4999999 as an explicit setting.
    const auto is_set = [](double v) { return v != 0.0; };
    const bool other_splits =
        std::any_of(other.split_shadow_color.begin(), other.split_shadow_color.end(), is_set) ||
        std::any_of(other.split_highlight_color.begin(), other.split_highlight_color.end(), is_set);
    for (int i = 0; i < 3; ++i) {
        self.split_shadow_color[i] =
            lim::split_color.clamp(self.split_shadow_color[i] + other.split_shadow_color[i]);
        self.split_highlight_color[i] =
            lim::split_color.clamp(self.split_highlight_color[i] + other.split_highlight_color[i]);
    }
    if (other_splits)
        self.split_balance = lim::split_balance.clamp(other.split_balance);

    // ASC CDL: slope/power multiplicative, offset additive, saturation multiplicative
    for (int i = 0; i < 3; ++i) {
        self.cdl_slope[i]  = lim::cdl_slope.clamp(self.cdl_slope[i] * other.cdl_slope[i]);
        self.cdl_offset[i] = lim::cdl_offset.clamp(self.cdl_offset[i] + other.cdl_offset[i]);
        self.cdl_power[i]  = lim::cdl_power.clamp(self.cdl_power[i] * other.cdl_power[i]);
    }
    self.cdl_saturation = lim::cdl_saturation.clamp(self.cdl_saturation * other.cdl_saturation);
}

bool is_default_perspective(const core::corners& perspective)
{
    return perspective.ul[0] == 0 && perspective.ul[1] == 0 && perspective.ur[0] == 1 && perspective.ur[1] == 0 &&
           perspective.ll[0] == 0 && perspective.ll[1] == 1 && perspective.lr[0] == 1 && perspective.lr[1] == 1;
}

draw_transforms draw_transforms::combine_transform(const core::image_transform& transform, double aspect_ratio) const
{
    draw_transforms new_transform(image_transform, steps);

    auto transform_before = new_transform.current().vertex_matrix;

    // Get matrix for turning coords in 'transform' into the parent frame.
    auto new_matrix = get_vertex_matrix(transform, aspect_ratio);

    apply_transform_colour_values(new_transform.image_transform, transform);
    new_transform.current().vertex_matrix = new_matrix * new_transform.current().vertex_matrix;

    // Only enable this for some transforms, to avoid applying crops when a draw_frame is just being used to flatten
    // other draw_frames
    if (transform.enable_geometry_modifiers) {
        // Push the new clip before the new transform applied
        draw_crop_region new_clip(transform.clip_translation[0],
                                  transform.clip_translation[1],
                                  transform.clip_translation[0] + transform.clip_scale[0],
                                  transform.clip_translation[1] + transform.clip_scale[1]);
        new_clip.apply_transform(transform_before);
        new_transform.current().crop_regions.push_back(std::move(new_clip));

        if (!is_default_perspective(transform.perspective)) {
            // Split into a new step
            new_transform.steps.emplace_back(transform.perspective,
                                             boost::numeric::ublas::identity_matrix<double>(3, 3));
        }

        // Push the new crop region with the new transform applied
        draw_crop_region new_crop(
            transform.crop.ul[0], transform.crop.ul[1], transform.crop.lr[0], transform.crop.lr[1]);
        new_crop.apply_transform(new_transform.current().vertex_matrix);
        new_transform.current().crop_regions.push_back(std::move(new_crop));
    }

    return std::move(new_transform);
}

void apply_perspective_to_vertex(t_point& vertex, const core::corners& perspective)
{
    const double x = vertex(0);
    const double y = vertex(1);

    // ul: x' =  (1-y) * a + (1 - a * (1-y)) * x
    vertex(0) += (1 - y) * perspective.ul[0] + (1 - perspective.ul[0] + perspective.ul[0] * y) * x - x;
    vertex(1) += (1 - x) * perspective.ul[1] + (1 - perspective.ul[1] + perspective.ul[1] * x) * y - y;

    // ur/ll: x' = x * (a * (1-y) + y)
    vertex(0) += x * (perspective.ur[0] * (1 - y) + y) - x;
    vertex(1) += y * (perspective.ll[1] * (1 - x) + x) - y;

    // ur/ll: x' = y * a + x * (1 - a * y)
    vertex(0) += y * perspective.ll[0] + x * (1 - perspective.ll[0] * y) - x;
    vertex(1) += x * perspective.ur[1] + y * (1 - perspective.ur[1] * x) - y;

    // lr: x' = x * (y * a + (1-y))
    vertex(0) += x * (y * perspective.lr[0] + (1 - y)) - x;
    vertex(1) += y * (x * perspective.lr[1] + (1 - x)) - y;
}

struct wrapped_vertex
{
    explicit wrapped_vertex(const core::frame_geometry::coord& coord)
    {
        vertex(0) = coord.vertex_x;
        vertex(1) = coord.vertex_y;
        vertex(2) = 1;

        texture_x = coord.texture_x;
        texture_y = coord.texture_y;
        texture_r = coord.texture_r;
        texture_q = coord.texture_q;
    }

    explicit wrapped_vertex() { vertex(2) = 1; };

    [[nodiscard]] core::frame_geometry::coord as_geometry() const
    {
        core::frame_geometry::coord res = {vertex(0), vertex(1), texture_x, texture_y};
        res.texture_r                   = texture_r;
        res.texture_q                   = texture_q;
        return res;
    }

    t_point vertex = t_point(3);

    double texture_x = 0.0;
    double texture_y = 0.0;
    double texture_r = 0.0;
    double texture_q = 1.0;
};

// Distance (in normalised coordinates) a vertex must be outside a crop edge to be cropped. Less than a pixel at 8k.
static const double epsilon = 0.0001;

bool inline point_is_outside_of_line(const t_point& line_1,
                                     const t_point& line_2,
                                     const t_point& vertex,
                                     bool           invert_winding)
{
    auto dx  = line_2(0) - line_1(0);
    auto dy  = line_2(1) - line_1(1);
    auto len = std::hypot(dx, dy);
    if (len == 0)
        return false;

    // Signed distance of the vertex from the crop edge. The cross product alone is scaled by the edge length, so
    // comparing it against epsilon would make the tolerance grow as the crop region gets smaller.
    auto dist = (dx * (vertex(1) - line_1(1)) - dy * (vertex(0) - line_1(0))) / len;
    return invert_winding ? dist > epsilon : dist < -epsilon;
}

// http://stackoverflow.com/questions/563198/how-do-you-detect-where-two-line-segments-intersect
bool get_intersection_with_crop_line(const t_point& crop0,
                                     const t_point& crop1,
                                     const t_point& p0,
                                     const t_point& p1,
                                     t_point&       result)
{
    double s1_x = crop1(0) - crop0(0);
    double s1_y = crop1(1) - crop0(1);
    double s2_x = p1(0) - p0(0);
    double s2_y = p1(1) - p0(1);

    double s = (-s1_y * (crop0(0) - p0(0)) + s1_x * (crop0(1) - p0(1))) / (-s2_x * s1_y + s1_x * s2_y);
    double t = (s2_x * (crop0(1) - p0(1)) - s2_y * (crop0(0) - p0(0))) / (-s2_x * s1_y + s1_x * s2_y);

    if (s >= 0 && s <= 1) {
        // Collision detected
        result(0) = crop0(0) + t * s1_x;
        result(1) = crop0(1) + t * s1_y;

        return true;
    }

    return false; // No collision
}

double calc_q(double close_diagonal, double distant_diagonal)
{
    return (close_diagonal + distant_diagonal) / distant_diagonal;
}

void crop_texture_for_vertex(const wrapped_vertex& line_a, const wrapped_vertex& line_b, wrapped_vertex& vertex)
{
    auto delta_point = vertex.vertex - line_a.vertex;
    auto delta_line  = line_b.vertex - line_a.vertex;

    // Calculate the dot product
    auto dot_product      = delta_point(0) * delta_line(0) + delta_point(1) * delta_line(1);
    auto line_len_squared = delta_line(0) * delta_line(0) + delta_line(1) * delta_line(1);

    // Skip if line has no length
    if (line_len_squared == 0) {
        vertex.texture_x = line_a.texture_x;
        vertex.texture_y = line_a.texture_y;
        return;
    }

    auto dist_delta = dot_product / line_len_squared;

    vertex.texture_x = line_a.texture_x + dist_delta * (line_b.texture_x - line_a.texture_x);
    vertex.texture_y = line_a.texture_y + dist_delta * (line_b.texture_y - line_a.texture_y);
    vertex.texture_q = line_a.texture_q + dist_delta * (line_b.texture_q - line_a.texture_q);
}

void fill_texture_q_for_quad(std::vector<wrapped_vertex>& coords)
{
    if (coords.size() != 4)
        return;

    // Based on formula from:
    // http://www.reedbeta.com/blog/2012/05/26/quadrilateral-interpolation-part-1/

    double s1_x = coords[2].vertex(0) - coords[0].vertex(0);
    double s1_y = coords[2].vertex(1) - coords[0].vertex(1);
    double s2_x = coords[3].vertex(0) - coords[1].vertex(0);
    double s2_y = coords[3].vertex(1) - coords[1].vertex(1);

    double s =
        (-s1_y * (coords[0].vertex(0) - coords[1].vertex(0)) + s1_x * (coords[0].vertex(1) - coords[1].vertex(1))) /
        (-s2_x * s1_y + s1_x * s2_y);
    double t =
        (s2_x * (coords[0].vertex(1) - coords[1].vertex(1)) - s2_y * (coords[0].vertex(0) - coords[1].vertex(0))) /
        (-s2_x * s1_y + s1_x * s2_y);

    if (s >= 0 && s <= 1 && t >= 0 && t <= 1) {
        // Collision detected
        double diagonal_intersection_x = coords[0].vertex(0) + t * s1_x;
        double diagonal_intersection_y = coords[0].vertex(1) + t * s1_y;

        auto d0 =
            std::hypot(diagonal_intersection_x - coords[3].vertex(0), diagonal_intersection_y - coords[3].vertex(1));
        auto d1 =
            std::hypot(diagonal_intersection_x - coords[2].vertex(0), diagonal_intersection_y - coords[2].vertex(1));
        auto d2 =
            std::hypot(diagonal_intersection_x - coords[1].vertex(0), diagonal_intersection_y - coords[1].vertex(1));
        auto d3 =
            std::hypot(diagonal_intersection_x - coords[0].vertex(0), diagonal_intersection_y - coords[0].vertex(1));

        auto ulq = calc_q(d3, d1);
        auto urq = calc_q(d2, d0);
        auto lrq = calc_q(d1, d3);
        auto llq = calc_q(d0, d2);

        std::vector<double> q_values = {ulq, urq, lrq, llq};

        int corner = 0;
        for (auto& coord : coords) {
            coord.texture_q = q_values[corner];
            coord.texture_x *= q_values[corner];
            coord.texture_y *= q_values[corner];

            if (++corner == 4)
                corner = 0;
        }
    }
}

void transform_vertex(const draw_transform_step& step, t_point& vertex)
{
    // Apply basic transforms of this step
    vertex = vertex * step.vertex_matrix;

    // Apply perspective. These rely on x and y of the coord, so can't be done as a shared matrix
    apply_perspective_to_vertex(vertex, step.perspective);
}

std::vector<core::frame_geometry::coord>
draw_transforms::transform_coords(const std::vector<core::frame_geometry::coord>& coords) const
{
    // Convert to matrix representations
    std::vector<wrapped_vertex> cropped_coords;
    cropped_coords.reserve(coords.size());

    for (const auto& coord : coords) {
        cropped_coords.emplace_back(coord);
    }

    std::vector<draw_crop_region> transformed_regions;

    // Apply the transforms
    for (int i = (int)steps.size() - 1; i >= 0; i--) {
        for (auto& coord : cropped_coords) {
            transform_vertex(steps[i], coord.vertex);
        }

        // Transform existing regions
        for (auto& region : transformed_regions) {
            for (int l = 0; l < 4; ++l) {
                transform_vertex(steps[i], region.coords[l]);
            }
        }

        // Push new regions
        for (auto& region : steps[i].crop_regions) {
            draw_crop_region new_region = region;
            for (int l = 0; l < 4; ++l) {
                // Only apply perspective for new ones
                apply_perspective_to_vertex(new_region.coords[l], steps[i].perspective);
            }

            transformed_regions.push_back(new_region);
        }
    }

    // Apply the perspective correction
    fill_texture_q_for_quad(cropped_coords);

    // Perform the crop
    for (auto& crop_region : transformed_regions) {
        // Determine the winding order of the crop region
        // Calculate the signed area to determine if the region is clockwise or counter-clockwise
        double signed_area = 0.0;
        for (int l = 0; l < 4; ++l) {
            int next_l = (l + 1) % 4;
            signed_area += (crop_region.coords[next_l](0) - crop_region.coords[l](0)) *
                           (crop_region.coords[next_l](1) + crop_region.coords[l](1));
        }
        // In screen coordinates (Y down), normal clockwise winding gives negative signed area
        // We need to invert the test when the winding is flipped (positive signed area)
        bool invert_winding = signed_area > 0;

        for (int l = 0; l < 4; ++l) {
            // Apply the crop, one edge at a time
            int     to_index   = l == 3 ? 0 : l + 1;
            t_point from_point = crop_region.coords[l];
            t_point to_point   = crop_region.coords[to_index];

            std::unordered_set<size_t> points_outside_of_line;

            // Figure out which points are outside the crop region
            for (size_t j = 0; j < cropped_coords.size(); ++j) {
                bool v = point_is_outside_of_line(from_point, to_point, cropped_coords[j].vertex, invert_winding);
                if (v)
                    points_outside_of_line.insert(j);
            }

            if (points_outside_of_line.empty()) {
                // Line has no effect, skip
                continue;
            } else if (points_outside_of_line.size() == cropped_coords.size()) {
                // All are outside, shape has no geometry
                return {};
            }

            std::vector<wrapped_vertex> new_coords;
            new_coords.reserve(cropped_coords.size() * 2); // Avoid reallocs for complex shapes

            // Iterate through the coords
            for (size_t j = 0; j < cropped_coords.size(); ++j) {
                if (points_outside_of_line.count(j) == 0) {
                    new_coords.push_back(cropped_coords[j]);
                    continue;
                }

                size_t prev_index = j == 0 ? cropped_coords.size() - 1 : j - 1;
                size_t next_index = j == cropped_coords.size() - 1 ? 0 : j + 1;

                bool prev_is_outside = points_outside_of_line.count(prev_index) == 1;
                bool next_is_outside = points_outside_of_line.count(next_index) == 1;

                if (prev_is_outside && next_is_outside) {
                    // Vertex and its edges are completely outside, skip
                    continue;
                }

                if (!prev_is_outside) {
                    // This edge intersects the crop line, calculate the new coordinates
                    wrapped_vertex new_coord;
                    if (get_intersection_with_crop_line(to_point,
                                                        from_point,
                                                        cropped_coords[prev_index].vertex,
                                                        cropped_coords[j].vertex,
                                                        new_coord.vertex)) {
                        crop_texture_for_vertex(cropped_coords[prev_index], cropped_coords[j], new_coord);
                        new_coords.emplace_back(std::move(new_coord));
                    } else {
                        // Geometry error! skip coordinate
                    }
                }

                if (!next_is_outside) {
                    // This edge intersects the crop line, calculate the new coordinates
                    wrapped_vertex new_coord;
                    if (get_intersection_with_crop_line(to_point,
                                                        from_point,
                                                        cropped_coords[j].vertex,
                                                        cropped_coords[next_index].vertex,
                                                        new_coord.vertex)) {
                        crop_texture_for_vertex(cropped_coords[j], cropped_coords[next_index], new_coord);
                        new_coords.emplace_back(std::move(new_coord));
                    } else {
                        // Geometry error! skip coordinate
                    }
                }
            }

            // Polygon is cropped, update state
            cropped_coords = new_coords;
        }

        {
            static const double pixel_epsilon = 0.0001; // less than a pixel at 8k

            // Prune duplicate coords
            std::vector<wrapped_vertex> new_coords;
            new_coords.reserve(cropped_coords.size()); // Avoid reallocs

            for (size_t j = 0; j < cropped_coords.size(); ++j) {
                size_t prev_index = j == 0 ? cropped_coords.size() - 1 : j - 1;

                auto delta = cropped_coords[j].vertex - cropped_coords[prev_index].vertex;
                if (std::abs(delta(0)) > pixel_epsilon || std::abs(delta(1)) > pixel_epsilon) {
                    new_coords.emplace_back(cropped_coords[j]);
                }
            }

            if (new_coords.size() < 3) {
                // Not enough coords to draw anything
                return {};
            }
            cropped_coords = new_coords;
        }
    }

    // Convert back to frame_geometry types
    std::vector<core::frame_geometry::coord> result;
    result.reserve(cropped_coords.size());
    for (auto& coord : cropped_coords) {
        result.push_back(coord.as_geometry());
    }

    return result;
}

} // namespace caspar::accelerator::ogl
