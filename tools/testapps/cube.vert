#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform Constants
{
    mat4 worldViewProj;
} ubo;

void main()
{
    gl_Position = ubo.worldViewProj * vec4(inPosition, 1.0);
    outColor = inColor;
}
