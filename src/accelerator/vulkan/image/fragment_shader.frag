#version 450

#extension GL_ARB_fragment_shader_interlock : enable
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec4 TexCoord;
layout(location = 1) in vec4 TexCoord2;

layout(location = 0) out vec4 fragColor;

// bindless texture array
layout(binding = 0) uniform sampler2D	textures[8];

layout(binding = 1, input_attachment_index = 0) uniform subpassInput background;

const uint PLANE0 = 0;
const uint PLANE1 = 1;
const uint PLANE2 = 2;
const uint PLANE3 = 3;
const uint LOCAL_KEY = 4;
const uint LAYER_KEY = 5;

const uint is_straight_alpha_mask = 1u << 0;
const uint has_local_key_mask = 1u << 1;
const uint has_layer_key_mask = 1u << 2;
const uint invert_mask = 1u << 3;
const uint levels_mask = 1u << 4;
const uint csb_mask = 1u << 5;
const uint chroma_mask = 1u << 6;
const uint chroma_show_mask_mask = 1u << 7;

layout(push_constant) uniform ParamsBlock {
    uint color_space_index;
    float precision_factor[4];
    int blend_mode;
    int keyer;
    int pixel_format;
    float opacity;

/* levels */
    float min_input;
    float max_input;
    float gamma;
    float min_output;
    float max_output;
    
/* contrast, saturation & brightness */
    float brt;
    float sat;
    float con;

/* Chroma */
    float chroma_target_hue;
    float chroma_hue_width;
    float chroma_min_saturation;
    float chroma_min_brightness;
    float chroma_softness;
    float chroma_spill_suppress;
    float chroma_spill_suppress_saturation;

    uint flags;
};

// Each matrix follows from its luma coefficients: Cr->R is 2(1-Kr), Cb->B is 2(1-Kb),
// and the green terms are -2(1-Kb)Kb/Kg and -2(1-Kr)Kr/Kg. BT.601's Cr->G was -0.509
// and should be -0.714136 = -2(1-0.299)(0.299)/0.587. Kept in step with the OpenGL
// mixer, which carried the same wrong literal.
const mat3[3] color_matrices = mat3[3](
                    mat3(1.0, 0.0, 1.402, 1.0, -0.344136, -0.714136, 1.0, 1.772, 0.0),
                    mat3(1.0, 0.0, 1.5748, 1.0, -0.1873, -0.4681, 1.0, 1.8556, 0.0),
                    mat3(1.0, 0.0, 1.4746, 1.0, -0.16455312684366, -0.57135312684366, 1.0, 1.8814, 0.0)
                ); 
const vec3[3] luma_coefficients = vec3[3](
                    vec3(0.299, 0.587, 0.114), // Rec. 601
                    vec3(0.2126, 0.7152, 0.0722),  // Rec. 709
                    vec3(0.2627, 0.6780, 0.0593)   // Rec. 2020
                );

/*
** Contrast, saturation, brightness
** Code of this function is from TGM's shader pack
** http://irrlicht.sourceforge.net/phpBB2/viewtopic.php?t=21057
*/

vec3 ContrastSaturationBrightness(vec4 color, float brt, float sat, float con)
{
    vec3 luma_coeff = luma_coefficients[color_space_index];
    
    const float AvgLumR = 0.5;
    const float AvgLumG = 0.5;
    const float AvgLumB = 0.5;

    vec3 AvgLumin = vec3(AvgLumR, AvgLumG, AvgLumB);
    vec3 brtColor = color.rgb * brt;
    vec3 intensity = vec3(dot(brtColor, luma_coeff));
    vec3 satColor = mix(intensity, brtColor, sat);
    vec3 conColor = mix(AvgLumin, satColor, con);

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
vec4 ChromaOnCustomColor(vec4 c, bool show_mask)
{
    vec3 hsv		= rgb2hsv(c.rgb);
    float distance	= ColorDistance(hsv);
    float d			= distance * -2.0 + 1.0;
    vec4 suppressed	= vec4(hsv2rgb(supress_spill(hsv)), 1.0);
    float alpha		= alpha_map(d);

    suppressed *= alpha;

    return show_mask ? vec4(suppressed.a, suppressed.a, suppressed.a, 1) : suppressed;
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
    // attachments have bgra format to match opengl accelerator
    vec4 back = subpassLoad(background).bgra;
    if(blend_mode != 0)
       fore.rgb = get_blend_color(back.rgb/(back.a+0.0000001), fore.rgb/(fore.a+0.0000001))*fore.a;
   switch(keyer)
   {
       case 1:  return fore + back; // additive
       default: return fore + (1.0-fore.a)*back; // linear
   }
}

vec4 ycbcra_to_rgba(float Y, float Cb, float Cr, float A)
{
    mat3 color_matrix = transpose(color_matrices[color_space_index]);

    const float luma_coefficient = 255.0/219.0;
    const float chroma_coefficient = 255.0/224.0;

    vec3 YCbCr = vec3(Y, Cb, Cr) * 255;
    YCbCr -= vec3(16.0, 128.0, 128.0);
    YCbCr *= vec3(luma_coefficient, chroma_coefficient, chroma_coefficient);

    return vec4(color_matrix * YCbCr / 255, A);
}

vec4 get_sample(sampler2D texSampler, vec2 coords)
{
    return texture(texSampler, coords);
}

vec4 get_rgba_color()
{
    vec2 uv = TexCoord.st / TexCoord.q;

    switch(pixel_format)
    {
    case 0:		//gray
        return vec4(get_sample(textures[PLANE0], uv).rrr * precision_factor[0], 1.0);
    case 1:		//bgra,
        return get_sample(textures[PLANE0], uv).bgra * precision_factor[0];
    case 2:		//rgba,
        return get_sample(textures[PLANE0], uv).rgba * precision_factor[0];
    case 3:		//argb,
        return get_sample(textures[PLANE0], uv).argb * precision_factor[0];
    case 4:		//abgr,
        return get_sample(textures[PLANE0], uv).gbar * precision_factor[0];
    case 5:		//ycbcr,
        {
            float y  = get_sample(textures[PLANE0], uv).r * precision_factor[0];
            float cb = get_sample(textures[PLANE1], uv).r * precision_factor[1];
            float cr = get_sample(textures[PLANE2], uv).r * precision_factor[2];
            return ycbcra_to_rgba(y, cb, cr, 1.0);
        }
    case 6:		//ycbcra
        {
            float y  = get_sample(textures[PLANE0], uv).r * precision_factor[0];
            float cb = get_sample(textures[PLANE1], uv).r * precision_factor[1];
            float cr = get_sample(textures[PLANE2], uv).r * precision_factor[2];
            float a  = get_sample(textures[PLANE3], uv).r * precision_factor[3];
            return ycbcra_to_rgba(y, cb, cr, a);
        }
    case 7:		//luma
        {
            vec3 y3 = get_sample(textures[PLANE0], uv).rrr * precision_factor[0];
            return vec4((y3-0.065)/0.859, 1.0);
        }
    case 8:		//bgr,
        return vec4(get_sample(textures[PLANE0], uv).bgr * precision_factor[0], 1.0);
    case 9:		//rgb,
        return vec4(get_sample(textures[PLANE0], uv).rgb * precision_factor[0], 1.0);
	case 10:	// uyvy
		{
			float y  = get_sample(textures[PLANE0], uv).g * precision_factor[0];
			float cb = get_sample(textures[PLANE1], uv).b * precision_factor[1];
			float cr = get_sample(textures[PLANE1], uv).r * precision_factor[1];
			return ycbcra_to_rgba(y, cb, cr, 1.0);
		}
    case 11:    // gbrp
        {
            float g = get_sample(textures[PLANE0], uv).r * precision_factor[0];
            float b = get_sample(textures[PLANE1], uv).r * precision_factor[1];
            float r = get_sample(textures[PLANE2], uv).r * precision_factor[2];
			return vec4(b, g, r, 1.0);
        }
    case 12:    // gbrap
        {
            float g = get_sample(textures[PLANE0], uv).r * precision_factor[0];
            float b = get_sample(textures[PLANE1], uv).r * precision_factor[1];
            float r = get_sample(textures[PLANE2], uv).r * precision_factor[2];
            float a = get_sample(textures[PLANE3], uv).r * precision_factor[3];
			return vec4(b, g, r, a);
        }
    }
    return vec4(0.0, 0.0, 0.0, 0.0);
}

void main()
{
    bool is_straight_alpha = (flags & is_straight_alpha_mask) == is_straight_alpha_mask;
    bool has_local_key = (flags & has_local_key_mask) == has_local_key_mask;
    bool has_layer_key  = (flags & has_layer_key_mask) == has_layer_key_mask;
    bool invert = (flags & invert_mask) == invert_mask;
    bool levels = (flags & levels_mask) == levels_mask;
    bool csb = (flags & csb_mask) == csb_mask;
    bool chroma = (flags & chroma_mask) == chroma_mask;
    bool chroma_show_mask = (flags & chroma_show_mask_mask) == chroma_show_mask_mask;

    vec4 color = get_rgba_color();
    if (is_straight_alpha)
        color.rgb *= color.a;
    if (chroma)
        color = ChromaOnCustomColor(color, chroma_show_mask);

    // The grading chain is non-linear, so it belongs on the surface colour: C(a*c) != a*C(c).
    bool graded = levels || csb;
    if (graded && color.a > 0.0)
        color.rgb /= color.a;

    if(levels)
        color.rgb = LevelsControl(color.rgb, min_input, gamma, max_input, min_output, max_output);
    if(csb)
        color.rgb = ContrastSaturationBrightness(color, brt, sat, con);

    if (graded)
        color.rgb *= color.a;

    if(has_local_key)
        color *= texture(textures[LOCAL_KEY], TexCoord2.st).r;
    if(has_layer_key)
        color *= texture(textures[LAYER_KEY], TexCoord2.st).r;
    color *= opacity;
    if (invert)
        color = 1.0 - color;
    if (blend_mode >= 0)
        color = blend(color);

    // attachments store color in bgra format to match opengl accelerator
    fragColor = color.bgra;
}
