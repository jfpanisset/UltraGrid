#version 130

#extension GL_EXT_gpu_shader4 : enable

#define lerp mix

// Image formats
const int FORMAT_RGB = 0;
const int FORMAT_YUV = 1;

// Covert YUV to RGB
vec3 ConvertYUVToRGB(vec3 color)
{
    float Y = color[0];
    float U = color[1] - 0.5;
    float V = color[2] - 0.5;
    Y = 1.1643 * (Y - 0.0625);

    float R = Y + 1.5958 * V;
    float G = Y - 0.39173 * U - 0.81290 * V;
    float B = Y + 2.017 * U;
    
    return vec3(R, G, B);
}

const float offset = 128.0 / 255.0;

vec3 ConvertRGBToYCoCg(vec3 color)
{
    float Y = (color.r + 2 * color.g + color.b) * 0.25;
    float Co = ( ( 2 * color.r - 2 * color.b      ) * 0.25 + offset );
    float Cg = ( (    -color.r + 2 * color.g - color.b) * 0.25 + offset );

    return vec3(Y, Co, Cg);
}

float colorDistance(vec3 c0, vec3 c1)
{
    return dot(c0-c1, c0-c1);
}
float colorDistance(vec2 c0, vec2 c1)
{
    return dot(c0-c1, c0-c1);
}

void ExtractColorBlockRGB(out vec3 col[16], sampler2D image, vec4 texcoord, vec2 imageSize)
{
    vec2 texelSize = (1.0f / imageSize);
    vec2 tex = vec2(texcoord.x, texcoord.y);
    tex -= texelSize * vec2(1.5);
    for ( int i = 0; i < 4; i++ ) {
        for ( int j = 0; j < 4; j++ ) {
            col[i * 4 + j] = ConvertRGBToYCoCg(texture(image, tex + vec2(j, i) * texelSize).rgb);
        }
    }
}

void ExtractColorBlockYUV(out vec3 col[16], sampler2D image, vec4 texcoord, vec2 imageSize)
{
    vec2 texelSize = (1.0f / imageSize);
    vec2 tex = vec2(texcoord.x, texcoord.y);
    tex -= texelSize * vec2(2);
    for ( int i = 0; i < 4; i++ ) {
        for ( int j = 0; j < 4; j++ ) {
            col[i * 4 + j] = ConvertRGBToYCoCg(ConvertYUVToRGB(texture(image, tex + vec2(j, i) * texelSize).rgb));
        }
    }
}

void FindMinMaxColorsBox(vec3 block[16], out vec3 mincol, out vec3 maxcol)
{
    mincol = block[0];
    maxcol = block[0];
    
    for ( int i = 1; i < 16; i++ ) {
        mincol = min(mincol, block[i]);
        maxcol = max(maxcol, block[i]);
    }
}

void InsetBBox(inout vec3 mincol, inout vec3 maxcol)
{
    vec3 inset = (maxcol - mincol) / 16.0 - (8.0 / 255.0) / 16;
    mincol = clamp(mincol + inset, 0.0, 1.0);
    maxcol = clamp(maxcol - inset, 0.0, 1.0);
}
void InsetYBBox(inout float mincol, inout float maxcol)
{
    float inset = (maxcol - mincol) / 32.0 - (16.0 / 255.0) / 32.0;
    mincol = clamp(mincol + inset, 0.0, 1.0);
    maxcol = clamp(maxcol - inset, 0.0, 1.0);
}
void InsetCoCgBBox(inout vec2 mincol, inout vec2 maxcol)
{
    vec2 inset = (maxcol - mincol) / 16.0 - (8.0 / 255.0) / 16;
    mincol = clamp(mincol + inset, 0.0, 1.0);
    maxcol = clamp(maxcol - inset, 0.0, 1.0);
}

void SelectDiagonal(vec3 block[16], inout vec3 mincol, inout vec3 maxcol)
{
    vec3 center = (mincol + maxcol) * 0.5;

    vec2 cov = vec2(0, 0);
    for (int i = 0; i < 16; i++) {
        vec3 t = block[i] - center;
        cov.x += t.x * t.z;
        cov.y += t.y * t.z;
    }

    if (cov.x < 0) {
        float temp = maxcol.x;
        maxcol.x = mincol.x;
        mincol.x = temp;
    }
    if (cov.y < 0) {
        float temp = maxcol.y;
        maxcol.y = mincol.y;
        mincol.y = temp;
    }
}

vec3 RoundAndExpand(vec3 v, out uint w)
{
    uvec3 c = uvec3(round(v * vec3(31, 63, 31)));
    w = (c.r << 11) | (c.g << 5) | c.b;

    c.rb = (c.rb << 3) | (c.rb >> 2);
    c.g = (c.g << 2) | (c.g >> 4);

    return vec3(c) * (1.0 / 255.0);
}

uint EmitEndPointsDXT1(inout vec3 mincol, inout vec3 maxcol)
{
    uvec2 outp;
    maxcol = RoundAndExpand(maxcol, outp.x);
    mincol = RoundAndExpand(mincol, outp.y);

    // We have to do this in case we select an alternate diagonal.
    if (outp.x < outp.y) {
        vec3 tmp = mincol;
        mincol = maxcol;
        maxcol = tmp;
        return outp.y | (outp.x << 16);
    }

    return outp.x | (outp.y << 16);
}

uint ScaleYCoCg(vec2 minColor, vec2 maxColor)
{
    vec2 m0 = abs(minColor - offset);
    vec2 m1 = abs(maxColor - offset);

    float m = max(max(m0.x, m0.y), max(m1.x, m1.y));

    const float s0 = 64.0 / 255.0;
    const float s1 = 32.0 / 255.0;

    uint scale = 1u;
    if ( m < s0 )
        scale = 2u;
    if ( m < s1 )
        scale = 4u;

    return scale;
}

void SelectYCoCgDiagonal(const vec3 block[16], inout vec2 minColor, inout vec2 maxColor)
{
    vec2 mid = (maxColor + minColor) * 0.5;

    float cov = 0;
    for ( int i = 0; i < 16; i++ ) {
        vec2 t = block[i].yz - mid;
        cov += t.x * t.y;
    }
    if ( cov < 0 ) {
        float tmp = maxColor.y;
        maxColor.y = minColor.y;
        minColor.y = tmp;
    }
}

uint EmitEndPointsYCoCgDXT5(inout vec2 mincol, inout vec2 maxcol, uint scale)
{
    maxcol = (maxcol - offset) * scale + offset;
    mincol = (mincol - offset) * scale + offset;

    InsetCoCgBBox(mincol, maxcol);

    maxcol = round(maxcol * vec2(31, 63));
    mincol = round(mincol * vec2(31, 63));

    uvec2 imaxcol = uvec2(maxcol);
    uvec2 imincol = uvec2(mincol);

    uvec2 outp;
    outp.x = (imaxcol.r << 11u) | (imaxcol.g << 5u) | (scale - uint(1));
    outp.y = (imincol.r << 11u) | (imincol.g << 5u) | (scale - uint(1));

    imaxcol.r = (imaxcol.r << 3u) | (imaxcol.r >> 2u);
    imaxcol.g = (imaxcol.g << 2u) | (imaxcol.g >> 4u);
    imincol.r = (imincol.r << 3u) | (imincol.r >> 2u);
    imincol.g = (imincol.g << 2u) | (imincol.g >> 4u);

    maxcol = vec2(imaxcol) * (1.0 / 255.0);
    mincol = vec2(imincol) * (1.0 / 255.0);

    // Undo rescale.
    maxcol = (maxcol - offset) / scale + offset;
    mincol = (mincol - offset) / scale + offset;

    return outp.x | (outp.y << 16u);
}

uint EmitIndicesYCoCgDXT5(vec3 block[16], vec2 mincol, vec2 maxcol)
{
    // Compute palette
    vec2 c[4];
    c[0] = maxcol;
    c[1] = mincol;
    c[2] = lerp(c[0], c[1], 1.0/3.0);
    c[3] = lerp(c[0], c[1], 2.0/3.0);

    // Compute indices
    uint indices = 0u;
    for ( int i = 0; i < 16; i++ )
    {
        // find index of closest color
        vec4 dist;
        dist.x = colorDistance(block[i].yz, c[0]);
        dist.y = colorDistance(block[i].yz, c[1]);
        dist.z = colorDistance(block[i].yz, c[2]);
        dist.w = colorDistance(block[i].yz, c[3]);

        uvec4 b;
        b.x = dist.x > dist.w ? 1u : 0u;
        b.y = dist.y > dist.z ? 1u : 0u;
        b.z = dist.x > dist.z ? 1u : 0u;
        b.w = dist.y > dist.w ? 1u : 0u;
        uint b4 = dist.z > dist.w ? 1u : 0u;

        uint index = (b.x & b4) | (((b.y & b.z) | (b.x & b.w)) << 1u);
        indices |= index << (uint(i) * 2u);
    }

    // Output indices
    return indices;
}

uint EmitAlphaEndPointsYCoCgDXT5(float mincol, float maxcol)
{
    uint c0 = uint(round(mincol * 255));
    uint c1 = uint(round(maxcol * 255));

    return (c0 << 8u) | c1;
}

// Version shown in the YCoCg-DXT article.
uvec2 EmitAlphaIndicesYCoCgDXT5(vec3 block[16], float minAlpha, float maxAlpha)
{
    const float ALPHA_RANGE = 7.0;

    float mid = (maxAlpha - minAlpha) / (2.0 * ALPHA_RANGE);

    float ab1 = minAlpha + mid;
    float ab2 = (6 * maxAlpha + 1 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;
    float ab3 = (5 * maxAlpha + 2 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;
    float ab4 = (4 * maxAlpha + 3 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;
    float ab5 = (3 * maxAlpha + 4 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;
    float ab6 = (2 * maxAlpha + 5 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;
    float ab7 = (1 * maxAlpha + 6 * minAlpha) * (1.0 / ALPHA_RANGE) + mid;

    uvec2 indices = uvec2(0, 0);

    uint index = 1u;
    for ( int i = 0; i < 6; i++ ) {
        float a = block[i].x;
        index = 1u;
        index += (a <= ab1) ? 1u : 0u;
        index += (a <= ab2) ? 1u : 0u;
        index += (a <= ab3) ? 1u : 0u;
        index += (a <= ab4) ? 1u : 0u;
        index += (a <= ab5) ? 1u : 0u;
        index += (a <= ab6) ? 1u : 0u;
        index += (a <= ab7) ? 1u : 0u;
        index &= 7u;
        index ^= (2u > index) ? 1u : 0u;
        indices.x |= index << (3u * uint(i) + 16u);
    }

    indices.y = index >> 1u;

    for ( int i = 6; i < 16; i++ ) {
        float a = block[i].x;
        index = 1u;
        index += (a <= ab1) ? 1u : 0u;
        index += (a <= ab2) ? 1u : 0u;
        index += (a <= ab3) ? 1u : 0u;
        index += (a <= ab4) ? 1u : 0u;
        index += (a <= ab5) ? 1u : 0u;
        index += (a <= ab6) ? 1u : 0u;
        index += (a <= ab7) ? 1u : 0u;
        index &= 7u;
        index ^= (2u > index) ? 1u : 0u;
        indices.y |= index << (3u * uint(i) - 16u);
    }

    return indices;
}

in vec4 TEX0;
uniform sampler2D image;
uniform int imageFormat = FORMAT_RGB;
uniform vec2 imageSize;
out uvec4 colorInt;

void main()
{
    // Read block of data
    vec3 block[16];
    if ( int(imageFormat) == FORMAT_YUV )
        ExtractColorBlockYUV(block, image, TEX0, imageSize);
    else
        ExtractColorBlockRGB(block, image, TEX0, imageSize);

    // Find min and max colors
    vec3 mincol, maxcol;
    FindMinMaxColorsBox(block, mincol, maxcol);

    SelectYCoCgDiagonal(block, mincol.yz, maxcol.yz);

    uint scale = ScaleYCoCg(mincol.yz, maxcol.yz);

    // Output CoCg in DXT1 block.
    uvec4 outp;
    outp.z = EmitEndPointsYCoCgDXT5(mincol.yz, maxcol.yz, scale);
    outp.w = EmitIndicesYCoCgDXT5(block, mincol.yz, maxcol.yz);

    InsetYBBox(mincol.x, maxcol.x);

    // Output Y in DXT5 alpha block.
    outp.x = EmitAlphaEndPointsYCoCgDXT5(mincol.x, maxcol.x);

    uvec2 indices = EmitAlphaIndicesYCoCgDXT5(block, mincol.x, maxcol.x);
    outp.x |= indices.x;
    outp.y = indices.y;
    
    colorInt = outp;
}
