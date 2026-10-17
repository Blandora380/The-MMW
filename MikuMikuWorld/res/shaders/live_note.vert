#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec4 aColor;
layout (location = 2) in vec3 aUV1;

out vec3 projectedUV;
out vec4 color;
uniform mat4 projection;

void main()
{
    // Ordinary stage quads use z=0. Tilted note quads carry (u*q, v*q, q).
    projectedUV = vec3(aUV1.xy, aUV1.z > 0.0 ? aUV1.z : 1.0);
    color = aColor;
    gl_Position = projection * vec4(aPos, 1.0);
}
