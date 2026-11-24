// Picture filters, ported from chiaki4ps5 (present.frag): detail restore, deband, sharpen, colour boost.
// Works on luma like the PS5 version, then shifts RGB by the luma change.
#include "fg_common.hlsli"
cbuffer FilterConstants : register(b1)
{
	float sharpen;  // 0..1
	float deband;   // 0 / 1
	float restore;  // 0, 1 (low), 2 (high)
	float boost;    // 0, 1, 2
	float time;
	float3 fpad;
};
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);

float L(int2 p) { return Luma(src.Load(int3(clamp(p, int2(0, 0), int2(fullSize) - 1), 0)).rgb); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if(id.x >= fullSize.x || id.y >= fullSize.y) return;
	int2 p = int2(id.xy);
	float3 c = src.Load(int3(p, 0)).rgb;
	float y0 = Luma(c);
	float y = y0;
	float n = L(p + int2(0, -1)), s = L(p + int2(0, 1)), e = L(p + int2(1, 0)), w = L(p + int2(-1, 0));

	// Detail restore: smooth compression noise/blocking but keep real edges.
	if(restore > 0.0)
	{
		float th = 0.015 + 0.02 * restore;
		float acc = y, wsum = 1.0;
		float nb[4] = { n, s, e, w };
		[unroll] for(int i = 0; i < 4; i++)
		{
			float d = abs(nb[i] - y);
			float wt = 0.35 * restore * saturate(1.0 - d / th);
			acc += nb[i] * wt;
			wsum += wt;
		}
		y = acc / wsum;
	}

	// Deband: flat gradients get a wider average plus a little dither.
	if(deband > 0.5)
	{
		float a = (L(p + int2(6, 6)) + L(p + int2(-6, 6)) + L(p + int2(6, -6)) + L(p + int2(-6, -6))) * 0.25;
		if(abs(a - y) < 0.006) y = lerp(y, a, 0.6);
		float dn = frac(sin(dot(float2(p) + time, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
		y += dn / 255.0;
	}

	// Sharpen: contrast-adaptive (CAS-style), stronger where it won't ring.
	if(sharpen > 0.0)
	{
		float mn = min(y, min(min(n, s), min(e, w)));
		float mx = max(y, max(max(n, s), max(e, w)));
		float amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
		float wg = -amp * lerp(0.04, 0.2, sharpen);
		y = saturate((y + (n + s + e + w) * wg) / (1.0 + 4.0 * wg));
	}

	c += (y - y0);

	// Colour boost: a bit more saturation and contrast.
	if(boost > 0.0)
	{
		float l = dot(c, float3(0.2126, 0.7152, 0.0722));
		c = lerp(l.xxx, c, 1.0 + 0.12 * boost);
		c = (c - 0.5) * (1.0 + 0.05 * boost) + 0.5;
	}
	dst[id.xy] = float4(saturate(c), 1.0);
}
