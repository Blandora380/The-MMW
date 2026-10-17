#version 330 core

in vec3 projectedUV;
in vec4 color;
out vec4 fragColor;

struct Material
{
    sampler2D diffuse0;
    sampler2D diffuse1;
    float blend;
};
uniform Material material;

void main()
{
    // Divide after interpolation so the tilted surface has no diagonal seam.
    vec2 uv = projectedUV.xy / max(projectedUV.z, 0.0001);
    fragColor = texture(material.diffuse0, uv) * color;
}
