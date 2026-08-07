// The plan document's section 3.3 dynamic input, kept as a language sample.
//
// It parses, lints and CI-gates today, but it does NOT generate an asset: putting HLSL on a Niagara
// custom node needs UNiagaraNodeCustomHlsl::SetCustomHlsl, which NiagaraEditor does not export, and
// the field behind it is private. Building this file reports DFX5100 saying so.
//
// Its equivalent, reachable today, is an inline hlsl { } expression at the point of use:
//
//     SpriteSizeMin = hlsl { pow(0.5 + 0.5 * sin(Engine.Time * 6.0 * 6.2831853), 2.0) }
//
// which is what NS_ToonHitSpark.dfs does.
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
