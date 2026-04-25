#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    float screenWidth;
    float screenHeight;
    float r, g, b, a;
} pc;

// Y plane — full-resolution luma (R8_UNORM)
layout(binding = 0) uniform sampler2D texY;
// UV plane — half-resolution chroma, interleaved (Cb,Cr) (RG8_UNORM)
layout(binding = 1) uniform sampler2D texUV;

// BT.709 limited-range (16–235 luma, 16–240 chroma) YCbCr → RGB
void main()
{
    float y   = texture(texY,  fragUV).r;
    vec2  uv  = texture(texUV, fragUV).rg;

    float Y  = (y    - 16.0  / 255.0) * (255.0 / 219.0);
    float Cb = (uv.r - 128.0 / 255.0) * (255.0 / 224.0);
    float Cr = (uv.g - 128.0 / 255.0) * (255.0 / 224.0);

    float r = clamp(Y + 1.5748 * Cr,               0.0, 1.0);
    float g = clamp(Y - 0.1873 * Cb - 0.4681 * Cr, 0.0, 1.0);
    float b = clamp(Y + 1.8556 * Cb,               0.0, 1.0);

    outColor = vec4(r, g, b, 1.0) * fragColor;
}
