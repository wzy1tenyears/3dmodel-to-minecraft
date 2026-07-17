struct FrameConstants {
    row_major float4x4 worldViewProjection;
    float4 normalColumn0;
    float4 normalColumn1;
    float4 normalColumn2;
    float4 lightDirectionAndAmbient;
};

[[vk::push_constant]] ConstantBuffer<FrameConstants> frame;

struct ModelInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR0;
};

struct ColorInput {
    float3 position : POSITION;
    float4 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR0;
};

struct PointVertexOutput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    [[vk::builtin("PointSize")]] float pointSize : PSIZE;
};

VertexOutput ModelVS(ModelInput input) {
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0f), frame.worldViewProjection);
    output.normal = normalize(float3(
        dot(input.normal, frame.normalColumn0.xyz),
        dot(input.normal, frame.normalColumn1.xyz),
        dot(input.normal, frame.normalColumn2.xyz)));
    output.color = input.color;
    return output;
}

VertexOutput ColorVS(ColorInput input) {
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0f), frame.worldViewProjection);
    output.normal = float3(0.0f, 1.0f, 0.0f);
    output.color = input.color;
    return output;
}

PointVertexOutput PointVS(ColorInput input) {
    PointVertexOutput output;
    output.position = mul(float4(input.position, 1.0f), frame.worldViewProjection);
    output.normal = float3(0.0f, 1.0f, 0.0f);
    output.color = input.color;
    output.pointSize = 3.0f;
    return output;
}

PointVertexOutput PointFallbackVS(ColorInput input) {
    PointVertexOutput output;
    output.position = mul(float4(input.position, 1.0f), frame.worldViewProjection);
    output.normal = float3(0.0f, 1.0f, 0.0f);
    output.color = input.color;
    output.pointSize = 1.0f;
    return output;
}

float4 LitPS(VertexOutput input) : SV_TARGET {
    const float diffuse = abs(dot(normalize(input.normal), normalize(-frame.lightDirectionAndAmbient.xyz)));
    const float light = saturate(frame.lightDirectionAndAmbient.w +
                                 diffuse * (1.0f - frame.lightDirectionAndAmbient.w));
    return float4(input.color.rgb * light, input.color.a);
}

float4 ColorPS(VertexOutput input) : SV_TARGET {
    return input.color;
}
