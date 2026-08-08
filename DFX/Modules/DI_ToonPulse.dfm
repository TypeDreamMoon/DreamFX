// The plan document's section 3.3 dynamic input, generated for real since plan-v2 W1.
//
// A DynamicInput body has to be a single expression, with or without the `return`. The Niagara
// translator wraps a dynamic input's custom HLSL as `Output = (Type)( <body> );`, so statements
// before the return would produce invalid HLSL rather than an error naming the real problem;
// DFX3037 catches that here instead. Multi-statement logic belongs in a Module -- see
// M_ToonSpin.dfm, whose body is emitted verbatim.
DynamicInput(Name="Modules/Moon/ToonPulse", Root="Plugin.DreamFX")
{
    Settings = {
        Usage    = DynamicInput;
        Output   = float;
        Category = "MoonToon|Math";
    }

    Inputs = {
        float Frequency = 6.0;
        float Sharpness = 2.0;
    }

    Body = {
        return pow(0.5 + 0.5 * sin(Engine.Time * Frequency * 6.2831853), Sharpness);
    }
}
