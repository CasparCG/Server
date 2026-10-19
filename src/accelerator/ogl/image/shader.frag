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

// Gamut compression, with ACES 1.3's limits
uniform bool		gamut_compress_enable;
// Per-channel limits in RGB order (cyan=red, magenta=green, yellow=blue).
uniform vec3		gc_limit;

// Secondary qualifier (HSL key + grade)
uniform bool		qualifier_enable;
uniform float		qual_target_hue;
uniform float		qual_hue_width;
uniform float		qual_min_sat;
uniform float		qual_max_sat;
uniform float		qual_min_lum;
uniform float		qual_max_lum;
uniform float		qual_softness;
uniform float		qual_exposure;
uniform float		qual_sat_offset;
uniform float		qual_hue_offset;

// Per-channel RGB Levels (independent levels per channel)
uniform bool		rgb_levels_enable;
uniform float		rgb_levels_min_input[3];
uniform float		rgb_levels_max_input[3];
uniform float		rgb_levels_gamma[3];
uniform float		rgb_levels_min_output[3];
uniform float		rgb_levels_max_output[3];

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

// Luma of the working colour, using the channel's own coefficients.
//
// luma_coeff is uploaded in RGB order and holds Rec.601, Rec.709 or Rec.2020 weights
// depending on the source (image_kernel.cpp), so a grade no longer hard-codes Rec.709
// and no longer disagrees with ContrastSaturationBrightness, which has always read this
// uniform. The `.bgr` is the working-order swizzle described above -- dot(c.bgr, k) and
// dot(c, k.bgr) are the same sum, and swizzling the three-element uniform rather than
// the pixel keeps this the only place the order is mentioned.
float working_luma(vec3 c) { return dot(c, luma_coeff.bgr); }

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

// ---- Gamut Compression ----
// Compresses out-of-gamut values toward the achromatic axis using a smooth
// toe function.  Operates per-channel on the distance from achromatic.
// An approximation with ACES 1.3's limits, not the ACES algorithm: one threshold
// rather than three, a rational curve rather than the power form, and all-negative
// pixels left untouched.
vec3 apply_gamut_compress(vec3 c, vec3 lim)
{
    float achromatic = max(max(c.r, c.g), max(c.b, 0.0));
    if (achromatic <= 0.0)
        return c;
    vec3  dist = (achromatic - c) / abs(achromatic);
    float thr  = 0.815;
    for (int i = 0; i < 3; ++i) {
        if (dist[i] > thr && lim[i] > 1.0001) {
            float nd = (dist[i] - thr) / (lim[i] - thr);
            float cd = nd / (1.0 + nd);
            dist[i]  = thr + cd * (lim[i] - thr);
        }
    }
    return achromatic - dist * abs(achromatic);
}

// ---- Secondary Qualifier (HSL key + grade) ----
// Isolates a colour range and applies exposure/saturation/hue corrections only
// to the qualified pixels, using soft masks for smooth transitions.
vec3 apply_qualifier(vec3 c, float tgt_hue, float hue_w, float min_s, float max_s,
                     float min_l, float max_l, float soft, float exp_off,
                     float sat_off, float hue_off)
{
    // c is BGR here -- this backend carries the pixel through grading in the mixer's
    // native BGR channel order, as gc_limit above and ContrastSaturationBrightness
    // already account for. rgb2hsv treats its first component as red, so feeding it
    // unswizzled puts the pixel's hue on the opposite side of the wheel from the
    // target the user asked for, and the qualifier keys the wrong colour entirely.
    vec3  hsv      = rgb2hsv(clamp(c.bgr, 0.0, 1.0));
    float hue_dist = AngleDiff(hsv.x, tgt_hue) * 2.0;
    float hue_mask = 1.0 - smoothstep(hue_w - soft, hue_w + soft, hue_dist);
    float sat_mask = smoothstep(min_s - soft, min_s + soft, hsv.y) *
                     (1.0 - smoothstep(max_s - soft, max_s + soft, hsv.y));
    float lum_mask = smoothstep(min_l - soft, min_l + soft, hsv.z) *
                     (1.0 - smoothstep(max_l - soft, max_l + soft, hsv.z));
    float mask     = hue_mask * sat_mask * lum_mask;
    if (mask < 0.001)
        return c;
    vec3 graded = c;
    graded *= (1.0 + exp_off);
    float glum = working_luma(graded);
    graded     = mix(vec3(glum), graded, 1.0 + sat_off);
    if (abs(hue_off) > 0.01) {
        // HDR-safe hue rotation: normalise by peak, rotate, re-scale.
        float peak = max(max(graded.r, graded.g), max(graded.b, 0.0001));
        vec3  ghsv = rgb2hsv(clamp((graded / peak).bgr, 0.0, 1.0));
        ghsv.x     = fract(ghsv.x + hue_off / 360.0);
        graded     = hsv2rgb(ghsv).bgr * peak;
    }
    return mix(c, graded, mask);
}

// ---- Per-channel RGB Levels ----
// Independent input range, gamma, and output range per channel.
// Uniforms are in RGB order; the working vector is BGR, so each channel reads its
// own RGB slot: c.r (blue) -> [2], c.g -> [1], c.b (red) -> [0].
vec3 apply_rgb_levels(vec3 c)
{
    c.r = clamp((c.r - rgb_levels_min_input[2]) / max(rgb_levels_max_input[2] - rgb_levels_min_input[2], 0.0001), 0.0, 1.0);
    c.r = pow(c.r, 1.0 / max(rgb_levels_gamma[2], 0.01));
    c.r = mix(rgb_levels_min_output[2], rgb_levels_max_output[2], c.r);
    c.g = clamp((c.g - rgb_levels_min_input[1]) / max(rgb_levels_max_input[1] - rgb_levels_min_input[1], 0.0001), 0.0, 1.0);
    c.g = pow(c.g, 1.0 / max(rgb_levels_gamma[1], 0.01));
    c.g = mix(rgb_levels_min_output[1], rgb_levels_max_output[1], c.g);
    c.b = clamp((c.b - rgb_levels_min_input[0]) / max(rgb_levels_max_input[0] - rgb_levels_min_input[0], 0.0001), 0.0, 1.0);
    c.b = pow(c.b, 1.0 / max(rgb_levels_gamma[0], 0.01));
    c.b = mix(rgb_levels_min_output[0], rgb_levels_max_output[0], c.b);
    return c;
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

void main()
{
    vec4 color = get_rgba_color();
    if (is_straight_alpha)
        color.rgb *= color.a;
    if (chroma)
        color = chroma_key(color);
    // Before the primaries, so the rest of the chain sees in-gamut colour.
    if (gamut_compress_enable)
        color.rgb = apply_gamut_compress(color.rgb, gc_limit.bgr);
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
    // Secondary: keys the already-graded image.
    if (qualifier_enable)
        color.rgb = apply_qualifier(color.rgb, qual_target_hue, qual_hue_width,
                                    qual_min_sat, qual_max_sat, qual_min_lum, qual_max_lum,
                                    qual_softness, qual_exposure, qual_sat_offset, qual_hue_offset);
    if (rgb_levels_enable)
        color.rgb = apply_rgb_levels(color.rgb);
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
