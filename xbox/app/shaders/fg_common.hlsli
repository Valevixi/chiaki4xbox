// Shared constants for the frame-generation compute shaders
cbuffer FgConstants : register(b0)
{
	uint2 fullSize;   // 1920x1080
	uint2 quarterSize; // 480x270 (luma pyramid used for motion search)
	uint2 blockCount; // motion vector grid (4x4 quarter-res blocks = 16x16 full-res)
	float occlusionK; // how quickly we fall back when the two warped samples disagree
	float pad0;
};

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
