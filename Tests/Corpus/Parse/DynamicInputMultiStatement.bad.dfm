// plan-v2 W1: the translator wraps a dynamic input body as `Output = (Type)( <body> );`, so statements
// before the return produce invalid HLSL rather than an error naming the real problem.
// EXPECT DFX3037
DynamicInput(Name="Corpus/DI_MultiStatement", Root="Plugin.DreamFX")
{
    Settings = {
        Usage  = DynamicInput;
        Output = float;
    }

    Inputs = {
        float Frequency = 1.0;
    }

    Body = {
        float Phase = Engine.Time * Frequency;
        return frac(Phase);
    }
}
