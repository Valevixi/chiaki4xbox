// Block-matching motion search at quarter res.
// For each 4x4 block of the CURRENT frame, find offset d so that cur(p) ~= prev(p + d).
// Output d in FULL-res pixels.
#include "fg_common.hlsli"
Texture2D<float> lumaPrev : register(t0);
Texture2D<float> lumaCur  : register(t1);
RWTexture2D<float2> mvOut : register(u0);

static const int R = 12; // +-12 quarter-res px = +-48 full-res px per 33 ms frame

float LoadQ(Texture2D<float> t, int2 p) { return t.Load(int3(clamp(p, int2(0, 0), int2(quarterSize) - 1), 0)); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if(id.x >= blockCount.x || id.y >= blockCount.y) return;
	int2 base = int2(id.xy) * 4;
	float cur[16];
	[unroll] for(int i = 0; i < 16; i++) cur[i] = LoadQ(lumaCur, base + int2(i % 4, i / 4));

	float best = 1e9; int2 bestD = int2(0, 0);
	for(int dy = -R; dy <= R; dy++)
	{
		for(int dx = -R; dx <= R; dx++)
		{
			float sad = 0;
			[unroll] for(int i = 0; i < 16; i++)
				sad += abs(cur[i] - LoadQ(lumaPrev, base + int2(i % 4, i / 4) + int2(dx, dy)));
			sad += 0.002 * (abs(dx) + abs(dy)); // prefer small/zero motion when ambiguous (HUD, flat areas)
			if(sad < best) { best = sad; bestD = int2(dx, dy); }
		}
	}
	mvOut[id.xy] = float2(bestD) * 4.0;
}
