// Build the in-between frame (t = 0.5) from prev and cur using the motion field.
// cur(x) ~= prev(x + d)  =>  content at mid-frame position m came from prev(m + d/2) and goes to cur(m - d/2).
// Includes the "hold" fix from chiaki4ps5: when the camera turns around a character it follows, the
// background matches perfectly on both sides of them and the motion search paints background through them.
// If the chosen motion explains neither real frame's own pixel at this spot, but staying still does, keep it still.
#include "fg_common.hlsli"
Texture2D<float4> rgbPrev : register(t0);
Texture2D<float4> rgbCur  : register(t1);
Texture2D<float2> mv      : register(t2);
SamplerState linearClamp  : register(s0);
RWTexture2D<float4> outRgb : register(u0);

static const float FG_HOLD = 0.12;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if(id.x >= fullSize.x || id.y >= fullSize.y) return;
	float2 size = float2(fullSize);
	float2 m = float2(id.xy) + 0.5;
	float2 d = mv.SampleLevel(linearClamp, m / (float2(blockCount) * 16.0), 0);

	float3 a = rgbPrev.SampleLevel(linearClamp, (m + d * 0.5) / size, 0).rgb;  // moved samples
	float3 b = rgbCur.SampleLevel(linearClamp, (m - d * 0.5) / size, 0).rgb;
	float3 a0 = rgbPrev.SampleLevel(linearClamp, m / size, 0).rgb;             // samples with no motion
	float3 b0 = rgbCur.SampleLevel(linearClamp, m / size, 0).rgb;

	float eMove = abs(Luma(a) - Luma(b));
	float eStill = abs(Luma(a0) - Luma(b0));
	// hold: the motion changes what both frames show at this spot, yet both frames agree there's no movement here
	bool hold = abs(Luma(a) - Luma(a0)) > FG_HOLD && abs(Luma(b) - Luma(b0)) > FG_HOLD && eStill < FG_HOLD;
	bool useStill = hold || eStill < eMove * 0.5;

	float3 warped = useStill ? 0.5 * (a0 + b0) : 0.5 * (a + b);
	float err = useStill ? eStill : eMove;
	// still disagree -> fall back to the newest real frame
	float w = saturate(1.0 - err * occlusionK);
	outRgb[id.xy] = float4(lerp(b0, warped, w), 1.0);
}
