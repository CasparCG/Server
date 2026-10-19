#version 450
in vec4 TexCoord;
in vec4 TexCoord2;
out vec4 fragColor;

uniform sampler2D	background;
uniform sampler2D	plane[4];
uniform sampler2D	local_key;
uniform sampler2D	layer_key;

uniform bool        is_straight_alpha;

uniform mat3		color_matrix;
uniform vec3		luma_coeff;
uniform bool		has_local_key;
uniform bool		has_layer_key;
uniform int			blend_mode;
uniform int			keyer;
uniform int			pixel_format;

uniform bool        invert;
uniform float		opacity;
uniform bool		levels;
uniform float		min_input;
uniform float		max_input;
uniform float		gamma;
uniform float		min_output;
uniform float		max_output;
uniform float	    precision_factor[4];

uniform bool		csb;
uniform float		brt;
uniform float		sat;
uniform float		con;

uniform bool		chroma;
uniform bool		chroma_show_mask;
uniform float		chroma_target_hue;
uniform float		chroma_hue_width;
uniform float		chroma_min_saturation;
uniform float		chroma_min_brightness;
uniform float		chroma_softness;
uniform float		chroma_spill_suppress;
uniform float		chroma_spill_suppress_saturation;

// White balance (temperature / tint)
uniform bool		white_balance;
uniform float		wb_temperature; // -1 cool (blue) .. +1 warm (orange)
uniform float		wb_tint;        // -1 magenta .. +1 green

// Lift / Midtone / Gain (3-way primary colour corrector)
// Per-channel uniforms are uploaded in RGB order and swizzled to the working order
// where they are used, in main() -- see the note above the grading functions below.
uniform bool		lmg_enable;
uniform vec3		lmg_lift;    // shadow offset per channel, default vec3(0)
uniform vec3		lmg_midtone; // midtone power per channel,  default vec3(1)
uniform vec3		lmg_gain;    // highlight multiplier per channel, default vec3(1)

// Hue shift
uniform bool		hue_shift_enable;
uniform float		hue_shift_degrees; // -180..+180

// Tonal balance (shadows / highlights separation)
uniform bool		tonebalance_enable;
uniform float		tb_shadows;    // -1..+1
uniform float		tb_highlights; // -1..+1

// Split toning (independent shadow / highlight tint)
uniform bool		split_tone_enable;
uniform vec3		split_shadow_color;
uniform vec3		split_highlight_color;
uniform float		split_balance; // crossover 0..1

// ASC CDL (Slope / Offset / Power)
uniform bool		cdl_enable;
uniform vec3		cdl_slope;
uniform vec3		cdl_offset;
uniform vec3		cdl_power;
uniform float		cdl_saturation;
// Color Grading (ACES color management)
uniform bool  color_grading;
uniform int   input_transfer;    // 0=linear,1=srgb,2=rec709,3=pq,4=hlg,5=logc3,6=slog3
uniform int   output_transfer;
uniform mat3  input_to_working;  // input gamut -> working space (ACEScg / AP1)
uniform mat3  working_to_output; // working space (ACEScg / AP1) -> output gamut
uniform int   tone_mapping_op;   // 0=none,1=reinhard,2=aces_filmic,3=aces_rrt,4=rrt_709,5=rrt_p3,6=rrt_2020_pq,7=hlg_ootf
uniform float exposure;          // linear exposure multiplier
uniform float luminance_scale;   // BT.2408 luminance adaptation across HDR/SDR domains
uniform float display_peak_luminance; // display nominal peak luminance in nits

/*
** Contrast, saturation, brightness
** Code of this function is from TGM's shader pack
** http://irrlicht.sourceforge.net/phpBB2/viewtopic.php?t=21057
*/

vec3 ContrastSaturationBrightness(vec4 color, float brt, float sat, float con)
{
    const float AvgLumR = 0.5;
    const float AvgLumG = 0.5;
    const float AvgLumB = 0.5;

    vec3 LumCoeff = luma_coeff.bgr;

    if (color.a > 0.0)
        color.rgb /= color.a;

    vec3 AvgLumin = vec3(AvgLumR, AvgLumG, AvgLumB);
    vec3 brtColor = color.rgb * brt;
    vec3 intensity = vec3(dot(brtColor, LumCoeff));
    vec3 satColor = mix(intensity, brtColor, sat);
    vec3 conColor = mix(AvgLumin, satColor, con);

    conColor.rgb *= color.a;

    return conColor;
}

/*
** Gamma correction
** Details: http://blog.mouaif.org/2009/01/22/photoshop-gamma-correction-shader/
*/
#define GammaCorrection(color, gamma)								pow(color, vec3(1.0 / gamma))

/*
** Levels control (input (+gamma), output)
** Details: http://blog.mouaif.org/2009/01/28/levels-control-shader/
*/

#define LevelsControlInputRange(color, minInput, maxInput)				min(max(color - vec3(minInput), vec3(0.0)) / (vec3(maxInput) - vec3(minInput)), vec3(1.0))
#define LevelsControlInput(color, minInput, gamma, maxInput)				GammaCorrection(LevelsControlInputRange(color, minInput, maxInput), gamma)
#define LevelsControlOutputRange(color, minOutput, maxOutput) 			mix(vec3(minOutput), vec3(maxOutput), color)
#define LevelsControl(color, minInput, gamma, maxInput, minOutput, maxOutput) 	LevelsControlOutputRange(LevelsControlInput(color, minInput, gamma, maxInput), minOutput, maxOutput)

/*
** Photoshop & misc math
** Blending modes, RGB/HSL/Contrast/Desaturate, levels control
**
** Romain Dura | Romz
** Blog: http://blog.mouaif.org
** Post: http://blog.mouaif.org/?p=94
*/


/*
** Desaturation
*/

vec4 Desaturate(vec3 color, float Desaturation)
{
    vec3 grayXfer = vec3(0.3, 0.59, 0.11);
    vec3 gray = vec3(dot(grayXfer, color));
    return vec4(mix(color, gray, Desaturation), 1.0);
}


/*
** Hue, saturation, luminance
*/

vec3 RGBToHSL(vec3 color)
{
    vec3 hsl;

    float fmin = min(min(color.r, color.g), color.b);
    float fmax = max(max(color.r, color.g), color.b);
    float delta = fmax - fmin;

    hsl.z = (fmax + fmin) / 2.0;

    if (delta == 0.0)
    {
        hsl.x = 0.0;
        hsl.y = 0.0;
    }
    else
    {
        if (hsl.z < 0.5)
            hsl.y = delta / (fmax + fmin);
        else
            hsl.y = delta / (2.0 - fmax - fmin);

        float deltaR = (((fmax - color.r) / 6.0) + (delta / 2.0)) / delta;
        float deltaG = (((fmax - color.g) / 6.0) + (delta / 2.0)) / delta;
        float deltaB = (((fmax - color.b) / 6.0) + (delta / 2.0)) / delta;

        if (color.r == fmax )
            hsl.x = deltaB - deltaG;
        else if (color.g == fmax)
            hsl.x = (1.0 / 3.0) + deltaR - deltaB;
        else if (color.b == fmax)
            hsl.x = (2.0 / 3.0) + deltaG - deltaR;

        if (hsl.x < 0.0)
            hsl.x += 1.0;
        else if (hsl.x > 1.0)
            hsl.x -= 1.0;
    }

    return hsl;
}

float HueToRGB(float f1, float f2, float hue)
{
    if (hue < 0.0)
        hue += 1.0;
    else if (hue > 1.0)
        hue -= 1.0;
    float res;
    if ((6.0 * hue) < 1.0)
        res = f1 + (f2 - f1) * 6.0 * hue;
    else if ((2.0 * hue) < 1.0)
        res = f2;
    else if ((3.0 * hue) < 2.0)
        res = f1 + (f2 - f1) * ((2.0 / 3.0) - hue) * 6.0;
    else
        res = f1;
    return res;
}

vec3 HSLToRGB(vec3 hsl)
{
    vec3 rgb;

    if (hsl.y == 0.0)
        rgb = vec3(hsl.z);
    else
    {
        float f2;

        if (hsl.z < 0.5)
            f2 = hsl.z * (1.0 + hsl.y);
        else
            f2 = (hsl.z + hsl.y) - (hsl.y * hsl.z);

        float f1 = 2.0 * hsl.z - f2;

        rgb.r = HueToRGB(f1, f2, hsl.x + (1.0/3.0));
        rgb.g = HueToRGB(f1, f2, hsl.x);
        rgb.b= HueToRGB(f1, f2, hsl.x - (1.0/3.0));
    }

    return rgb;
}




/*
** Float blending modes
** Adapted from here: http://www.nathanm.com/photoshop-blending-math/
** But I modified the HardMix (wrong condition), Overlay, SoftLight, ColorDodge, ColorBurn, VividLight, PinLight (inverted layers) ones to have correct results
*/

#define BlendLinearDodgef 					BlendAddf
#define BlendLinearBurnf 					BlendSubstractf
#define BlendAddf(base, blend) 				min(base + blend, 1.0)
#define BlendSubstractf(base, blend) 		max(base + blend - 1.0, 0.0)
#define BlendLightenf(base, blend) 		max(blend, base)
#define BlendDarkenf(base, blend) 			min(blend, base)
#define BlendLinearLightf(base, blend) 	(blend < 0.5 ? BlendLinearBurnf(base, (2.0 * blend)) : BlendLinearDodgef(base, (2.0 * (blend - 0.5))))
#define BlendScreenf(base, blend) 			(1.0 - ((1.0 - base) * (1.0 - blend)))
#define BlendOverlayf(base, blend) 		(base < 0.5 ? (2.0 * base * blend) : (1.0 - 2.0 * (1.0 - base) * (1.0 - blend)))
#define BlendSoftLightf(base, blend) 		((blend < 0.5) ? (2.0 * base * blend + base * base * (1.0 - 2.0 * blend)) : (sqrt(base) * (2.0 * blend - 1.0) + 2.0 * base * (1.0 - blend)))
#define BlendColorDodgef(base, blend) 		((blend == 1.0) ? blend : min(base / (1.0 - blend), 1.0))
#define BlendColorBurnf(base, blend) 		((blend == 0.0) ? blend : max((1.0 - ((1.0 - base) / blend)), 0.0))
#define BlendVividLightf(base, blend)		((blend < 0.5) ? BlendColorBurnf(base, (2.0 * blend)) : BlendColorDodgef(base, (2.0 * (blend - 0.5))))
#define BlendPinLightf(base, blend) 		((blend < 0.5) ? BlendDarkenf(base, (2.0 * blend)) : BlendLightenf(base, (2.0 *(blend - 0.5))))
#define BlendHardMixf(base, blend) 		((BlendVividLightf(base, blend) < 0.5) ? 0.0 : 1.0)
#define BlendReflectf(base, blend) 		((blend == 1.0) ? blend : min(base * base / (1.0 - blend), 1.0))


/*
** Vector3 blending modes
*/

#define Blend(base, blend, funcf) 			vec3(funcf(base.r, blend.r), funcf(base.g, blend.g), funcf(base.b, blend.b))

#define BlendNormal(base, blend) 			(blend)
#define BlendLighten						BlendLightenf
#define BlendDarken						BlendDarkenf
#define BlendMultiply(base, blend) 		(base * blend)
#define BlendAverage(base, blend) 			((base + blend) / 2.0)
#define BlendAdd(base, blend) 				min(base + blend, vec3(1.0))
#define BlendSubstract(base, blend) 		max(base + blend - vec3(1.0), vec3(0.0))
#define BlendDifference(base, blend) 		abs(base - blend)
#define BlendNegation(base, blend) 		(vec3(1.0) - abs(vec3(1.0) - base - blend))
#define BlendExclusion(base, blend) 		(base + blend - 2.0 * base * blend)
#define BlendScreen(base, blend) 			Blend(base, blend, BlendScreenf)
#define BlendOverlay(base, blend) 			Blend(base, blend, BlendOverlayf)
#define BlendSoftLight(base, blend) 		Blend(base, blend, BlendSoftLightf)
#define BlendHardLight(base, blend) 		BlendOverlay(blend, base)
#define BlendColorDodge(base, blend) 		Blend(base, blend, BlendColorDodgef)
#define BlendColorBurn(base, blend) 		Blend(base, blend, BlendColorBurnf)
#define BlendLinearDodge					BlendAdd
#define BlendLinearBurn					BlendSubstract
#define BlendLinearLight(base, blend) 		Blend(base, blend, BlendLinearLightf)
#define BlendVividLight(base, blend) 		Blend(base, blend, BlendVividLightf)
#define BlendPinLight(base, blend) 		Blend(base, blend, BlendPinLightf)
#define BlendHardMix(base, blend) 			Blend(base, blend, BlendHardMixf)
#define BlendReflect(base, blend) 			Blend(base, blend, BlendReflectf)
#define BlendGlow(base, blend) 			BlendReflect(blend, base)
#define BlendPhoenix(base, blend) 			(min(base, blend) - max(base, blend) + vec3(1.0))
#define BlendOpacity(base, blend, F, O) 	(F(base, blend) * O + blend * (1.0 - O))


vec3 BlendHue(vec3 base, vec3 blend)
{
    vec3 baseHSL = RGBToHSL(base);
    return HSLToRGB(vec3(RGBToHSL(blend).r, baseHSL.g, baseHSL.b));
}

vec3 BlendSaturation(vec3 base, vec3 blend)
{
    vec3 baseHSL = RGBToHSL(base);
    return HSLToRGB(vec3(baseHSL.r, RGBToHSL(blend).g, baseHSL.b));
}

vec3 BlendColor(vec3 base, vec3 blend)
{
    vec3 blendHSL = RGBToHSL(blend);
    return HSLToRGB(vec3(blendHSL.r, blendHSL.g, RGBToHSL(base).b));
}

vec3 BlendLuminosity(vec3 base, vec3 blend)
{
    vec3 baseHSL = RGBToHSL(base);
    return HSLToRGB(vec3(baseHSL.r, baseHSL.g, RGBToHSL(blend).b));
}

// Chroma keying
// Author: Tim Eves <timseves@googlemail.com>
//
// This implements the Chroma key algorithm described in the paper:
//      'Software Chroma Keying in an Imersive Virtual Environment'
//      by F. van den Bergh & V. Lalioti
// but as a pixel shader algorithm.
//
vec4  grey_xfer  = vec4(luma_coeff, 0);

// This allows us to implement the paper's alphaMap curve in software
// rather than a largeish array
float alpha_map(float d)
{
    return 1.0 - smoothstep(1.0, chroma_softness, d);
}

// http://stackoverflow.com/questions/15095909/from-rgb-to-hsv-in-opengl-glsl
vec3 rgb2hsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// From the same page
vec3 hsv2rgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

float AngleDiff(float angle1, float angle2)
{
    return 0.5 - abs(abs(angle1 - angle2) - 0.5);
}

float AngleDiffDirectional(float angle1, float angle2)
{
    float diff = angle1 - angle2;

    return diff < -0.5
            ? diff + 1.0
            : (diff > 0.5 ? diff - 1.0 : diff);
}

float Distance(float actual, float target)
{
    return min(0.0, target - actual);
}

float ColorDistance(vec3 hsv)
{
    float hueDiff					= AngleDiff(hsv.x, chroma_target_hue) * 2;
    float saturationDiff			= Distance(hsv.y, chroma_min_saturation);
    float brightnessDiff			= Distance(hsv.z, chroma_min_brightness);

    float saturationBrightnessScore	= max(brightnessDiff, saturationDiff);
    float hueScore					= hueDiff - chroma_hue_width;

    return -hueScore * saturationBrightnessScore;
}

vec3 supress_spill(vec3 c)
{
    float hue		= c.x;
    float diff		= AngleDiffDirectional(hue, chroma_target_hue);
    float distance	= abs(diff) / chroma_spill_suppress;

    if (distance < 1)
    {
        c.x = diff < 0
                ? chroma_target_hue - chroma_spill_suppress
                : chroma_target_hue + chroma_spill_suppress;
        c.y *= min(1.0, distance + chroma_spill_suppress_saturation);
    }

    return c;
}

// Key on any color
vec4 ChromaOnCustomColor(vec4 c)
{
    vec3 hsv		= rgb2hsv(c.rgb);
    float distance	= ColorDistance(hsv);
    float d			= distance * -2.0 + 1.0;
    vec4 suppressed	= vec4(hsv2rgb(supress_spill(hsv)), 1.0);
    float alpha		= alpha_map(d);

    suppressed *= alpha;

    return chroma_show_mask ? vec4(suppressed.a, suppressed.a, suppressed.a, 1) : suppressed;
}



vec3 get_blend_color(vec3 back, vec3 fore)
{
    switch(blend_mode)
    {
    case  0: return BlendNormal(back, fore);
    case  1: return BlendLighten(back, fore);
    case  2: return BlendDarken(back, fore);
    case  3: return BlendMultiply(back, fore);
    case  4: return BlendAverage(back, fore);
    case  5: return BlendAdd(back, fore);
    case  6: return BlendSubstract(back, fore);
    case  7: return BlendDifference(back, fore);
    case  8: return BlendNegation(back, fore);
    case  9: return BlendExclusion(back, fore);
    case 10: return BlendScreen(back, fore);
    case 11: return BlendOverlay(back, fore);
//	case 12: return BlendSoftLight(back, fore);
    case 13: return BlendHardLight(back, fore);
    case 14: return BlendColorDodge(back, fore);
    case 15: return BlendColorBurn(back, fore);
    case 16: return BlendLinearDodge(back, fore);
    case 17: return BlendLinearBurn(back, fore);
    case 18: return BlendLinearLight(back, fore);
    case 19: return BlendVividLight(back, fore);
    case 20: return BlendPinLight(back, fore);
    case 21: return BlendHardMix(back, fore);
    case 22: return BlendReflect(back, fore);
    case 23: return BlendGlow(back, fore);
    case 24: return BlendPhoenix(back, fore);
    case 25: return BlendHue(back, fore);
    case 26: return BlendSaturation(back, fore);
    case 27: return BlendColor(back, fore);
    case 28: return BlendLuminosity(back, fore);
    }
    return BlendNormal(back, fore);
}

vec4 blend(vec4 fore)
{
    vec4 back = texture(background, TexCoord2.st).bgra;
    if(blend_mode != 0)
        fore.rgb = get_blend_color(back.rgb/(back.a+0.0000001), fore.rgb/(fore.a+0.0000001))*fore.a;
    switch(keyer)
    {
        case 1:  return fore + back; // additive
        default: return fore + (1.0-fore.a)*back; // linear
    }
}

vec4 chroma_key(vec4 c)
{
    return ChromaOnCustomColor(c.bgra).bgra;
}

vec4 ycbcra_to_rgba(float Y, float Cb, float Cr, float A)
{
    const float luma_coefficient = 255.0/219.0;
    const float chroma_coefficient = 255.0/224.0;

    vec3 YCbCr = vec3(Y, Cb, Cr) * 255;
    YCbCr -= vec3(16.0, 128.0, 128.0);
    YCbCr *= vec3(luma_coefficient, chroma_coefficient, chroma_coefficient);

    return vec4(color_matrix * YCbCr / 255, A).bgra;
}

vec4 get_sample(sampler2D sampler, vec2 coords)
{
    return texture(sampler, coords);
}

vec4 get_rgba_color()
{
    switch(pixel_format)
    {
    case 0:		//gray
        return vec4(get_sample(plane[0], TexCoord.st / TexCoord.q).rrr * precision_factor[0], 1.0);
    case 1:		//bgra,
        return get_sample(plane[0], TexCoord.st / TexCoord.q).bgra * precision_factor[0];
    case 2:		//rgba,
        return get_sample(plane[0], TexCoord.st / TexCoord.q).rgba * precision_factor[0];
    case 3:		//argb,
        return get_sample(plane[0], TexCoord.st / TexCoord.q).argb * precision_factor[0];
    case 4:		//abgr,
        return get_sample(plane[0], TexCoord.st / TexCoord.q).gbar * precision_factor[0];
    case 5:		//ycbcr,
        {
            float y  = get_sample(plane[0], TexCoord.st / TexCoord.q).r * precision_factor[0];
            float cb = get_sample(plane[1], TexCoord.st / TexCoord.q).r * precision_factor[1];
            float cr = get_sample(plane[2], TexCoord.st / TexCoord.q).r * precision_factor[2];
            return ycbcra_to_rgba(y, cb, cr, 1.0);
        }
    case 6:		//ycbcra
        {
            float y  = get_sample(plane[0], TexCoord.st / TexCoord.q).r * precision_factor[0];
            float cb = get_sample(plane[1], TexCoord.st / TexCoord.q).r * precision_factor[1];
            float cr = get_sample(plane[2], TexCoord.st / TexCoord.q).r * precision_factor[2];
            float a  = get_sample(plane[3], TexCoord.st / TexCoord.q).r * precision_factor[3];
            return ycbcra_to_rgba(y, cb, cr, a);
        }
    case 7:		//luma
        {
            vec3 y3 = get_sample(plane[0], TexCoord.st / TexCoord.q).rrr * precision_factor[0];
            return vec4((y3-0.065)/0.859, 1.0);
        }
    case 8:		//bgr,
        return vec4(get_sample(plane[0], TexCoord.st / TexCoord.q).bgr * precision_factor[0], 1.0);
    case 9:		//rgb,
        return vec4(get_sample(plane[0], TexCoord.st / TexCoord.q).rgb * precision_factor[0], 1.0);
	case 10:	// uyvy
		{
			float y = get_sample(plane[0], TexCoord.st / TexCoord.q).g * precision_factor[0];
			float cb = get_sample(plane[1], TexCoord.st / TexCoord.q).b * precision_factor[1];
			float cr = get_sample(plane[1], TexCoord.st / TexCoord.q).r * precision_factor[1];
			return ycbcra_to_rgba(y, cb, cr, 1.0);
		}
    case 11:    // gbrp
        {
            float g  = get_sample(plane[0], TexCoord.st / TexCoord.q).r * precision_factor[0];
            float b = get_sample(plane[1], TexCoord.st / TexCoord.q).r * precision_factor[1];
            float r = get_sample(plane[2], TexCoord.st / TexCoord.q).r * precision_factor[2];
			return vec4(b, g, r, 1.0);
        }
    case 12:    // gbrap
        {
            float g  = get_sample(plane[0], TexCoord.st / TexCoord.q).r * precision_factor[0];
            float b = get_sample(plane[1], TexCoord.st / TexCoord.q).r * precision_factor[1];
            float r = get_sample(plane[2], TexCoord.st / TexCoord.q).r * precision_factor[2];
            float a  = get_sample(plane[3], TexCoord.st / TexCoord.q).r * precision_factor[3];
			return vec4(b, g, r, a);
        }
    }
    return vec4(0.0, 0.0, 0.0, 0.0);
}

// ============================ Primary grading chain ============================
//
// CHANNEL ORDER. The colour vector carried through this shader is in BGR order:
// `color.r` holds displayed blue. That is this shader's own long-standing
// convention, not something the grading chain introduced -- see `fragColor =
// color.bgra` at the end of main(), which swaps back on output, and
// `LumCoeff = luma_coeff.bgr` in ContrastSaturationBrightness above, which
// swizzles an RGB-order uniform at its point of use.
//
// The grading functions below therefore follow the same rule as
// ContrastSaturationBrightness: they take and return the working (BGR) vector,
// and anything that needs a real RGB triple -- a Rec.709 luma dot product, a hue
// conversion, a per-channel uniform -- swizzles it at the point of use. All
// per-channel uniforms are uploaded from C++ in plain RGB order and get their
// `.bgr` in main(), so the convention lives in exactly one file.
//
// Note that greys are invariant under a red/blue exchange, so a grey ramp passes
// every channel-order defect in here. Test with asymmetric per-channel values.
// ===============================================================================

// Luma of the working colour, using the channel's own coefficients.
//
// luma_coeff is uploaded in RGB order and holds Rec.601, Rec.709 or Rec.2020 weights
// depending on the source (image_kernel.cpp), so a grade no longer hard-codes Rec.709
// and no longer disagrees with ContrastSaturationBrightness, which has always read this
// uniform. The `.bgr` is the working-order swizzle described above -- dot(c.bgr, k) and
// dot(c, k.bgr) are the same sum, and swizzling the three-element uniform rather than
// the pixel keeps this the only place the order is mentioned.
float working_luma(vec3 c) { return dot(c, luma_coeff.bgr); }

// ---- White Balance ----
// temp: -1=cool (blue), +1=warm (orange); tint_val: -1=magenta, +1=green.
// Diagonal gain matrix, no clamping so HDR headroom is preserved.
vec3 apply_white_balance(vec3 c, float temp, float tint_val)
{
    vec3 gain_rgb = vec3(1.0 + temp     * 0.20,  // warm -> more red
                         1.0 + tint_val * 0.10,  // tint -> more green
                         1.0 - temp     * 0.20); // warm -> less blue
    return c * gain_rgb.bgr;
}

// ---- Lift / Midtone / Gain (3-way colour corrector) ----
// out = pow(max(c * gain + lift, 0.0), 1.0 / midtone).
// midtone is clamped before the reciprocal, not after: at midtone == 0 the
// reciprocal is +Inf and pow() collapses the channel to 0 (or Inf above 1.0),
// and clamping the exponent cannot undo that -- max(0.01, Inf) is Inf. A
// negative midtone would likewise invert the curve. Clamping the base to
// [0.01, 100] bounds the exponent to [0.01, 100] and covers both.
// No upper clamp on the colour so HDR values pass through; negatives clamped for pow() safety.
vec3 apply_lmg(vec3 c, vec3 lift, vec3 midtone, vec3 gain)
{
    c = max(c * gain + lift, vec3(0.0));
    c = pow(c, 1.0 / clamp(midtone, vec3(0.01), vec3(100.0)));
    return c;
}

// ---- Hue Shift ----
// Rotate hue in HSV space.  Extract peak luminance, normalise, rotate, re-scale
// so HDR values (>1.0) are preserved.
// rgb2hsv does not know what the channels mean; it treats the first as red, and
// exchanging red and blue mirrors the hue wheel. Feeding it the working vector
// unswizzled negated every rotation: a requested +15 degrees came out as -15, and only
// 0 and 180 looked right because they are their own negations. Saturation and value are
// unchanged by the exchange, which is why nothing else about this function was wrong.
vec3 apply_hue_shift(vec3 c, float degrees)
{
    float peak = max(max(c.r, c.g), max(c.b, 0.0001));
    vec3  norm = c / peak;
    vec3  hsv  = rgb2hsv(clamp(norm.bgr, 0.0, 1.0));
    hsv.x      = fract(hsv.x + degrees / 360.0);
    return hsv2rgb(hsv).bgr * peak;
}

// ---- Tonal Balance (Shadows / Highlights separation) ----
// Soft luminance-based masks drive additive offsets.  No clamping so HDR
// headroom is preserved for downstream tonemapping.
vec3 apply_tone_balance(vec3 c, float shadows, float highlights)
{
    float lum         = working_luma(c);
    float shadow_mask = 1.0 - smoothstep(0.0, 0.6, lum);
    float hl_mask     = smoothstep(0.4, 1.0, lum);
    c += vec3(shadows    * 0.5 * shadow_mask);
    c += vec3(highlights * 0.5 * hl_mask);
    return c;
}

// ---- Split Toning ----
// Luminance-based tint: additive colour offsets for shadows and highlights,
// with a crossover controlled by bal.
vec3 apply_split_tone(vec3 c, vec3 shad_col, vec3 hi_col, float bal)
{
    float lum     = working_luma(c);
    float shad_mk = 1.0 - smoothstep(bal - 0.3, bal + 0.3, lum);
    float hi_mk   = smoothstep(bal - 0.3, bal + 0.3, lum);
    c += shad_col * shad_mk;
    c += hi_col   * hi_mk;
    return c;
}

// ---- ASC CDL (Slope/Offset/Power) ----
// Industry-standard primary grade: out = pow(max(in * slope + offset, 0), power),
// followed by a saturation adjustment.
vec3 apply_cdl(vec3 c, vec3 slope, vec3 off, vec3 pwr, float sat_val)
{
    c = pow(max(c * slope + off, vec3(0.0)), pwr);
    float lum = working_luma(c);
    c = mix(vec3(lum), c, sat_val);
    return c;
}

// ---- Color Grading: EOTFs (encoded -> linear) ----
float eotf_srgb(float x)   { return x <= 0.04045  ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4); }
float eotf_rec709(float x) { return pow(max(x, 0.0), 2.4); }  // BT.1886 display EOTF
float eotf_pq(float x) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float xp = pow(max(x, 0.0), 1.0 / m2);
    return pow(max(xp - c1, 0.0) / (c2 - c3 * xp), 1.0 / m1);
}
float eotf_hlg(float x) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    return x <= 0.5 ? (x * x) / 3.0 : (exp((x - c) / a) + b) / 12.0;
}
float eotf_logc3(float x) {
    const float a = 5.555556, b = 0.052272, c = 0.247190, d = 0.385537, e = 5.367655, f = 0.092809;
    return x > e * 0.010591 + f ? (pow(10.0, (x - d) / c) - b) / a : (x - f) / e;
}
float eotf_slog3(float x) {
    const float cut = 171.2102946929 / 1023.0;
    return x >= cut ? pow(10.0, (x - 0.410557184750733) / 0.341132524981570) * 0.18 + 0.01
                    : (x - 95.0 / 1023.0) * 0.01 / (cut - 95.0 / 1023.0);
}
float eotf_gamma24(float x) { return pow(max(x, 0.0), 2.4); }  // Pure gamma 2.4 inverse
float eotf_gamma26(float x) { return pow(max(x, 0.0), 2.6); }  // Pure gamma 2.6 inverse
vec3 apply_eotf(vec3 rgb, int t) {
    switch (t) {
        case 1: return vec3(eotf_srgb(rgb.r),  eotf_srgb(rgb.g),  eotf_srgb(rgb.b));
        case 2: return vec3(eotf_rec709(rgb.r), eotf_rec709(rgb.g), eotf_rec709(rgb.b));
        case 3: return vec3(eotf_pq(rgb.r),    eotf_pq(rgb.g),    eotf_pq(rgb.b));
        case 4: return vec3(eotf_hlg(rgb.r),   eotf_hlg(rgb.g),   eotf_hlg(rgb.b));
        case 5: return vec3(eotf_logc3(rgb.r), eotf_logc3(rgb.g), eotf_logc3(rgb.b));
        case 6: return vec3(eotf_slog3(rgb.r), eotf_slog3(rgb.g), eotf_slog3(rgb.b));
        case 7: return rgb;  // linear (no EOTF needed)
        case 8: return vec3(eotf_gamma24(rgb.r), eotf_gamma24(rgb.g), eotf_gamma24(rgb.b));
        case 9: return vec3(eotf_gamma26(rgb.r), eotf_gamma26(rgb.g), eotf_gamma26(rgb.b));
        default: return rgb;
    }
}
// ---- Color Grading: OETFs (linear -> encoded) ----
float oetf_srgb(float x)   { return x <= 0.0031308 ? x * 12.92 : 1.055 * pow(max(x, 0.0), 1.0 / 2.4) - 0.055; }
float oetf_rec709(float x) { return pow(max(x, 0.0), 1.0 / 2.4); }  // BT.1886 inverse
float oetf_pq(float x) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float xn = pow(clamp(x, 0.0, 1.0), m1);
    return pow((c1 + c2 * xn) / (1.0 + c3 * xn), m2);
}
float oetf_hlg(float x) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    x = max(x, 0.0);
    return x <= 1.0 / 12.0 ? sqrt(3.0 * x) : a * log(12.0 * x - b) + c;
}
float oetf_gamma24(float x) { return pow(max(x, 0.0), 1.0 / 2.4); }  // Pure gamma 2.4 (EBU)
float oetf_gamma26(float x) { return pow(max(x, 0.0), 1.0 / 2.6); }  // Pure gamma 2.6 (DCI)
vec3 apply_oetf(vec3 rgb, int t) {
    rgb = max(rgb, vec3(0.0));
    switch (t) {
        case 1: return vec3(oetf_srgb(rgb.r),  oetf_srgb(rgb.g),  oetf_srgb(rgb.b));
        case 2: return vec3(oetf_rec709(rgb.r), oetf_rec709(rgb.g), oetf_rec709(rgb.b));
        case 3: return vec3(oetf_pq(rgb.r),    oetf_pq(rgb.g),    oetf_pq(rgb.b));
        case 4: return vec3(oetf_hlg(rgb.r),   oetf_hlg(rgb.g),   oetf_hlg(rgb.b));
        case 5: return rgb;  // linear (no OETF)
        case 6: return vec3(oetf_gamma24(rgb.r), oetf_gamma24(rgb.g), oetf_gamma24(rgb.b));
        case 7: return vec3(oetf_gamma26(rgb.r), oetf_gamma26(rgb.g), oetf_gamma26(rgb.b));
        default: return rgb;
    }
}
// ---- Color Grading: Tone Mapping ----
vec3 tonemap_reinhard(vec3 v)    { return v / (v + 1.0); }
vec3 tonemap_aces_filmic(vec3 x) { return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14), 0.0, 1.0); }
vec3 tonemap_aces_rrt(vec3 v) {
    v *= 0.6;
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.432951) + 0.238081;
    return clamp(a / b, 0.0, 1.0);
}

// HLG OOTF: BT.2100 system gamma for display rendering.
// gamma = 1.2 * 1.111^log2(Lw/1000), where Lw = display peak luminance in nits.
// Includes soft-knee highlight rolloff for sub-1000-nit displays to avoid hard clipping.
vec3 tonemap_hlg_ootf(vec3 v, float npl) {
    float gamma = 1.2 * pow(1.111, log2(npl / 1000.0));
    float Ys = dot(v, vec3(0.2627, 0.6780, 0.0593));
    vec3 result = v * pow(max(Ys, 1e-6), gamma - 1.0);
    if (npl < 1000.0) {
        float Yd = dot(result, vec3(0.2627, 0.6780, 0.0593));
        float knee = 0.85;
        if (Yd > knee) {
            float compressed = knee + (1.0 - knee) * tanh((Yd - knee) / (1.0 - knee));
            result *= compressed / max(Yd, 1e-6);
        }
    }
    return result;
}

// log10 is not a GLSL built-in; define it via natural log.
float log10(float x) { return log(x) * 0.4342944819032518; }

// ---- ACES Reference Rendering Transform (RRT) + Output Display Transform (ODT) ----
// Segmented spline fit from the ACES CTL reference implementation.
// This is the c5 spline used in the RRT.
float segmented_spline_c5_fwd(float x) {
    const float coefsLow[6] = float[6](-4.0000000000, -4.0000000000, -3.1573765773, -0.4852499958, 1.8477324706, 1.8477324706);
    const float coefsHigh[6] = float[6](-0.7185482425, 2.0810307172, 3.6681241237, 4.0000000000, 4.0000000000, 4.0000000000);
    const float minPoint_x = 0.0001; const float minPoint_y = 0.0001;
    const float midPoint_x = 0.18;   const float midPoint_y = 4.8;
    const float maxPoint_x = 65504.0; const float maxPoint_y = 10000.0;
    const int N_KNOTS_LOW = 4; const int N_KNOTS_HIGH = 4;

    float xCheck = clamp(x, minPoint_x, maxPoint_x);
    float logx = log10(xCheck);
    float logy;

    if (logx <= log10(midPoint_x)) {
        float knot_coord = (logx - log10(minPoint_x)) / (log10(midPoint_x) - log10(minPoint_x)) * float(N_KNOTS_LOW);
        int j = int(clamp(knot_coord, 0.0, float(N_KNOTS_LOW - 1)));
        float t = clamp(knot_coord - float(j), 0.0, 1.0);
        float cf0 = coefsLow[j]; float cf1 = coefsLow[j+1]; float cf2 = coefsLow[min(j+2, 5)];
        float c0 = mix(cf0, cf1, 0.5); float c1 = cf1; float c2 = mix(cf1, cf2, 0.5);
        logy = mix(mix(c0, c1, t), mix(c1, c2, t), t);
    } else {
        float knot_coord = (logx - log10(midPoint_x)) / (log10(maxPoint_x) - log10(midPoint_x)) * float(N_KNOTS_HIGH);
        int j = int(clamp(knot_coord, 0.0, float(N_KNOTS_HIGH - 1)));
        float t = clamp(knot_coord - float(j), 0.0, 1.0);
        float cf0 = coefsHigh[j]; float cf1 = coefsHigh[j+1]; float cf2 = coefsHigh[min(j+2, 5)];
        float c0 = mix(cf0, cf1, 0.5); float c1 = cf1; float c2 = mix(cf1, cf2, 0.5);
        logy = mix(mix(c0, c1, t), mix(c1, c2, t), t);
    }
    return pow(10.0, logy);
}

// c9 spline used in the ODTs
float segmented_spline_c9_fwd(float x, float minPt_y, float midPt_y, float maxPt_y) {
    const float coefsLow[10] = float[10](-1.6989700043, -1.6989700043, -1.4779000000, -1.2291000000, -0.8648000000, -0.4480000000, 0.0051800000, 0.4511080334, 0.9113744414, 0.9113744414);
    const float coefsHigh[10] = float[10](0.5154386965, 0.8470437783, 1.1358000000, 1.3802000000, 1.5197000000, 1.5985000000, 1.6467000000, 1.6746091357, 1.6878733390, 1.6878733390);
    const int N_KNOTS_LOW = 8; const int N_KNOTS_HIGH = 8;
    const float minPt_x = 0.18 * exp2(-6.5);
    const float midPt_x = 0.18;
    const float maxPt_x = 0.18 * exp2(6.5);

    float xCheck = clamp(x, minPt_x, maxPt_x);
    float logx = log10(xCheck);
    float logy;

    if (logx <= log10(midPt_x)) {
        float knot_coord = (logx - log10(minPt_x)) / (log10(midPt_x) - log10(minPt_x)) * float(N_KNOTS_LOW);
        int j = int(clamp(knot_coord, 0.0, float(N_KNOTS_LOW - 1)));
        float t = clamp(knot_coord - float(j), 0.0, 1.0);
        float cf0 = coefsLow[j]; float cf1 = coefsLow[j+1]; float cf2 = coefsLow[min(j+2, 9)];
        float c0 = mix(cf0, cf1, 0.5); float c1 = cf1; float c2 = mix(cf1, cf2, 0.5);
        logy = mix(mix(c0, c1, t), mix(c1, c2, t), t);
    } else {
        float knot_coord = (logx - log10(midPt_x)) / (log10(maxPt_x) - log10(midPt_x)) * float(N_KNOTS_HIGH);
        int j = int(clamp(knot_coord, 0.0, float(N_KNOTS_HIGH - 1)));
        float t = clamp(knot_coord - float(j), 0.0, 1.0);
        float cf0 = coefsHigh[j]; float cf1 = coefsHigh[j+1]; float cf2 = coefsHigh[min(j+2, 9)];
        float c0 = mix(cf0, cf1, 0.5); float c1 = cf1; float c2 = mix(cf1, cf2, 0.5);
        logy = mix(mix(c0, c1, t), mix(c1, c2, t), t);
    }
    return pow(10.0, logy);
}

// ACES RRT: apply segmented spline c5 per channel (in ACES AP1 / ACEScg)
vec3 aces_rrt(vec3 v) {
    return vec3(segmented_spline_c5_fwd(v.r),
                segmented_spline_c5_fwd(v.g),
                segmented_spline_c5_fwd(v.b));
}

// ACES ODT for sRGB/Rec.709 100 nit display
vec3 aces_odt_srgb(vec3 v) {
    v = vec3(segmented_spline_c9_fwd(v.r, 0.0001, 4.8, 48.0),
             segmented_spline_c9_fwd(v.g, 0.0001, 4.8, 48.0),
             segmented_spline_c9_fwd(v.b, 0.0001, 4.8, 48.0));
    v /= 48.0;
    return clamp(v, 0.0, 1.0);
}

// ACES RRT+ODT combined for Rec.709 SDR display (op=4)
vec3 tonemap_aces_rrt_709(vec3 v) {
    v = aces_rrt(v);
    return aces_odt_srgb(v);
}

// ACES RRT+ODT for P3-D65 display (op=5)
vec3 tonemap_aces_rrt_p3(vec3 v) {
    v = aces_rrt(v);
    v = vec3(segmented_spline_c9_fwd(v.r, 0.0001, 4.8, 48.0),
             segmented_spline_c9_fwd(v.g, 0.0001, 4.8, 48.0),
             segmented_spline_c9_fwd(v.b, 0.0001, 4.8, 48.0));
    v /= 48.0;
    return clamp(v, 0.0, 1.0);
}

// ACES RRT+ODT for Rec.2020 1000-nit HDR PQ display (op=6)
vec3 tonemap_aces_rrt_2020_pq(vec3 v) {
    v = aces_rrt(v);
    v = vec3(segmented_spline_c9_fwd(v.r, 0.005, 4.8, 800.0),
             segmented_spline_c9_fwd(v.g, 0.005, 4.8, 800.0),
             segmented_spline_c9_fwd(v.b, 0.005, 4.8, 800.0));
    v /= 1000.0;
    return clamp(v, 0.0, 1.0);
}

vec3 apply_tone_mapping(vec3 rgb, int op) {
    switch (op) {
        case 1: return tonemap_reinhard(rgb);
        case 2: return tonemap_aces_filmic(rgb);
        case 3: return tonemap_aces_rrt(rgb);
        case 4: return tonemap_aces_rrt_709(rgb);
        case 5: return tonemap_aces_rrt_p3(rgb);
        case 6: return tonemap_aces_rrt_2020_pq(rgb);
        case 7: return tonemap_hlg_ootf(rgb, display_peak_luminance);
        default: return rgb;
    }
}

void main()
{
    vec4 color = get_rgba_color();
    if (is_straight_alpha)
        color.rgb *= color.a;
    if (chroma)
        color = chroma_key(color);
    // Input conversion, before the grading chain: the chain must operate in the
    // working space.
    if (color_grading) {
        color.rgb  = apply_eotf(color.rgb, input_transfer);
        color.rgb *= luminance_scale;
        color.bgr  = input_to_working * color.bgr;
        color.rgb *= exposure;
    }

    // Per-channel uniforms arrive in RGB order and are swizzled to the working order
    // here -- see the channel-order note above the grading functions.
    if (cdl_enable)
        color.rgb = apply_cdl(color.rgb, cdl_slope.bgr, cdl_offset.bgr, cdl_power.bgr, cdl_saturation);
    if (white_balance)
        color.rgb = apply_white_balance(color.rgb, wb_temperature, wb_tint);
    if (lmg_enable)
        color.rgb = apply_lmg(color.rgb, lmg_lift.bgr, lmg_midtone.bgr, lmg_gain.bgr);
    if (hue_shift_enable)
        color.rgb = apply_hue_shift(color.rgb, hue_shift_degrees);
    if (tonebalance_enable)
        color.rgb = apply_tone_balance(color.rgb, tb_shadows, tb_highlights);
    if (split_tone_enable)
        color.rgb =
            apply_split_tone(color.rgb, split_shadow_color.bgr, split_highlight_color.bgr, split_balance);
    // Output half: tone map from the working space, then encode for the display.
    if (color_grading) {
        if (tone_mapping_op > 0)
            color.rgb = apply_tone_mapping(color.rgb, tone_mapping_op);
        color.bgr  = working_to_output * color.bgr;
        if (tone_mapping_op == 0)
            color.rgb = clamp(color.rgb, 0.0, 1.0);
        color.rgb  = apply_oetf(color.rgb, output_transfer);
    }
    if(levels)
        color.rgb = LevelsControl(color.rgb, min_input, gamma, max_input, min_output, max_output);
    if(csb)
        color.rgb = ContrastSaturationBrightness(color, brt, sat, con);
    if(has_local_key)
        color *= texture(local_key, TexCoord2.st).r;
    if(has_layer_key)
        color *= texture(layer_key, TexCoord2.st).r;
    color *= opacity;
    if (invert)
        color = 1.0 - color;
    if (blend_mode >= 0)
        color = blend(color);
    fragColor = color.bgra;
}
