#version 330 core

// Bilinear SRGGB10P debayer, ported from telefacet-web/src/webgl/Debayer.js
// (fragmentShaderQuality). The input texture stores the raw packed bytes as
// GL_R8 with width = bytes_per_line and height = image height.
//
// 4 pixels are packed into 5 bytes:
//   bytes[0..3] = upper 8 bits of pixels 0..3
//   bytes[4]    = lower 2 bits per pixel, [7:6][5:4][3:2][1:0]

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_texture;
uniform vec2  u_textureSize;   // image (width, height) in pixels
uniform float u_bytesPerLine;  // texture width in bytes
uniform vec3  u_awbGains;

float unpack10bit(vec2 imageCoord) {
    float x = floor(imageCoord.x);
    float y = floor(imageCoord.y);
    if (x < 0.0 || y < 0.0 || x >= u_textureSize.x || y >= u_textureSize.y) {
        return 0.0;
    }

    float groupIdx = floor(x / 4.0);
    int   pixelInGroup = int(mod(x, 4.0));

    float rowStart   = y * u_bytesPerLine;
    float groupStart = groupIdx * 5.0;

    float bytePos[5];
    bytePos[0] = rowStart + groupStart;
    bytePos[1] = rowStart + groupStart + 1.0;
    bytePos[2] = rowStart + groupStart + 2.0;
    bytePos[3] = rowStart + groupStart + 3.0;
    bytePos[4] = rowStart + groupStart + 4.0;

    float bytes[5];
    for (int i = 0; i < 5; ++i) {
        float col = mod(bytePos[i], u_bytesPerLine);
        float row = floor(bytePos[i] / u_bytesPerLine);
        vec2  tc  = vec2((col + 0.5) / u_bytesPerLine,
                         (row + 0.5) / u_textureSize.y);
        bytes[i] = texture(u_texture, tc).r * 255.0;
    }

    float v;
    if (pixelInGroup == 0) {
        v = bytes[0] * 4.0 + floor(bytes[4] / 64.0);
    } else if (pixelInGroup == 1) {
        v = bytes[1] * 4.0 + floor(mod(bytes[4], 64.0) / 16.0);
    } else if (pixelInGroup == 2) {
        v = bytes[2] * 4.0 + floor(mod(bytes[4], 16.0) / 4.0);
    } else {
        v = bytes[3] * 4.0 + mod(bytes[4], 4.0);
    }
    return v / 1023.0;
}

float sampleAt(vec2 c) { return unpack10bit(c); }

void main() {
    vec2 imageCoord = v_texCoord * u_textureSize;
    vec2 pixelCoord = floor(imageCoord);
    vec2 alt = mod(pixelCoord, 2.0);

    float C  = sampleAt(pixelCoord);
    float N  = sampleAt(pixelCoord + vec2( 0.0, -1.0));
    float S  = sampleAt(pixelCoord + vec2( 0.0,  1.0));
    float E  = sampleAt(pixelCoord + vec2( 1.0,  0.0));
    float W  = sampleAt(pixelCoord + vec2(-1.0,  0.0));
    float NE = sampleAt(pixelCoord + vec2( 1.0, -1.0));
    float NW = sampleAt(pixelCoord + vec2(-1.0, -1.0));
    float SE = sampleAt(pixelCoord + vec2( 1.0,  1.0));
    float SW = sampleAt(pixelCoord + vec2(-1.0,  1.0));

    vec3 color;
    if (alt.y < 0.5) {
        if (alt.x < 0.5) {
            // Red at (0,0)
            color = vec3(C, (N + S + E + W) * 0.25, (NW + NE + SW + SE) * 0.25);
        } else {
            // Green in red row
            color = vec3((W + E) * 0.5, C, (N + S) * 0.5);
        }
    } else {
        if (alt.x < 0.5) {
            // Green in blue row
            color = vec3((N + S) * 0.5, C, (W + E) * 0.5);
        } else {
            // Blue at (1,1)
            color = vec3((NW + NE + SW + SE) * 0.25, (N + S + E + W) * 0.25, C);
        }
    }

    color *= u_awbGains;
    fragColor = vec4(color, 1.0);
}
