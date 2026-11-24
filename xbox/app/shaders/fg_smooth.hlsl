// 3x3 median filter on the motion field (removes outlier vectors)
#include "fg_common.hlsli"
Texture2D<float2> mvIn : register(t0);
RWTexture2D<float2> mvOut : register(u0);

float Median9(float v[9])
{
	// partial sort network for median of 9
	[unroll] for(int i = 0; i < 5; i++)
		[unroll] for(int j = i + 1; j < 9; j++)
			if(v[j] < v[i]) { float t = v[i]; v[i] = v[j]; v[j] = t; }
	return v[4];
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if(id.x >= blockCount.x || id.y >= blockCount.y) return;
	float xs[9], ys[9];
	[unroll] for(int i = 0; i < 9; i++)
	{
		int2 p = clamp(int2(id.xy) + int2(i % 3 - 1, i / 3 - 1), int2(0, 0), int2(blockCount) - 1);
		float2 v = mvIn.Load(int3(p, 0));
		xs[i] = v.x; ys[i] = v.y;
	}
	mvOut[id.xy] = float2(Median9(xs), Median9(ys));
}
