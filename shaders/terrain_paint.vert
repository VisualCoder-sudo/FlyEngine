#version 330

in vec3 vertexPosition;
in vec3 vertexNormal;
in vec2 vertexTexCoord;

uniform mat4 mvp;
uniform mat4 matModel;

out vec3 worldPos;
out vec3 worldNormal;
out vec2 texCoord;

void main() {
    vec4 worldPos4 = matModel * vec4(vertexPosition, 1.0);
    worldPos = worldPos4.xyz;
    mat3 normalMatrix = mat3(transpose(inverse(matModel)));
    worldNormal = normalize(normalMatrix * vertexNormal);
    texCoord = vertexTexCoord;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
