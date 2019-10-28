#version 450

layout(push_constant) uniform push {
	layout(offset = 0) vec2 dst_origin;
	layout(offset = 8) vec2 src_origin;
	layout(offset = 16) vec2 dst_scale;
};

layout(location = 0) in ivec2 pos;
layout(location = 0) out vec2 src_pos;

void main() {
	gl_Position = vec4(dst_scale * (dst_origin + pos) - vec2(1, 1), 0, 1);
	src_pos = src_origin + pos;
}
