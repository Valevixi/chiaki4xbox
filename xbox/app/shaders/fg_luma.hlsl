// RGB frame (full res) -> average luma at quarter res
#include "fg_common.hlsli"
Texture2D<float4> srcRgb : register(t0);
RWTexture2D<float> dstLuma : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if(id.x >= quarterSize.x || id.y >= quarterSize.y) return;
	uint2 base = id.xy * 4;
	float sum = 0;
	[unroll] for(uint y = 0; y < 4; y++)
		[unroll] for(uint x = 0; x < 4; x++)
			sum += Luma(srcRgb.Load(int3(min(base + uint2(x, y), fullSize - 1), 0)).rgb);
	dstLuma[id.xy] = sum / 16.0;
}
