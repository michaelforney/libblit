#version 450

layout(push_constant) uniform push {
	layout(offset = 32) vec4 in_color;
};

layout(location = 0) out vec4 color;

void main() {
	color = in_color;
}
