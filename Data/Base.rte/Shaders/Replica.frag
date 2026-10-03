#version 330 core
#extension GL_KHR_blend_equation_advanced: enable
#ifdef GL_KHR_blend_equation_advanced
layout(blend_support_all_equations) out;
#endif
in vec2 textureUV;
in vec4 vertexColor;
out vec4 FragColor;
uniform sampler2D rteTexture;
uniform sampler2D rtePalette;
uniform bool drawMasked;
uniform bool trueColor;
uniform bool solidColor;
uniform float paletteColor;
uniform vec4 rteColor = vec4(1.0);
uniform bool rteBlendInvert = false;
uniform bool dissolve = false;
void main() {
    vec4 sampleColor = texture(rteTexture, textureUV);
    if (trueColor) {
        if (drawMasked && (sampleColor.a == 0.0 || (sampleColor.r == 1.0 && sampleColor.g == 0.0 && sampleColor.b == 1.0))) discard;
        FragColor = sampleColor * vertexColor * rteColor;
    } else {
        float index = sampleColor.r;
        if (drawMasked && index == 0.0) discard;
        FragColor = texture(rtePalette, vec2(solidColor ? paletteColor : index * vertexColor.r, 0.0));
        FragColor.a *= vertexColor.a;
        FragColor *= rteColor;
    }
    if (rteBlendInvert) FragColor.rgb = vec3(1.0) - FragColor.rgb;
    if (dissolve && fract(sin(dot(gl_FragCoord.xy, vec2(12.9898,78.233))) * 43758.5453) > FragColor.a) discard;
}
