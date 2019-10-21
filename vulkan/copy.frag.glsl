#version 450

layout(binding = 0) uniform sampler2D src;
layout(location = 0) in noperspective vec2 src_pos;
layout(location = 0) out vec4 color;

void main() {
	color = texture(src, src_pos);
}
