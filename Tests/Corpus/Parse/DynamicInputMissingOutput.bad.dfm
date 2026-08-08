// A dynamic input's return type cannot be inferred from its body, so it has to be declared.
// EXPECT DFX3031
DynamicInput(Name="Corpus/DI_MissingOutput", Root="Plugin.DreamFX")
{
    Settings = {
        Usage = DynamicInput;
    }

    Inputs = {
        float Frequency = 1.0;
    }

    Body = {
        return sin(Engine.Time * Frequency);
    }
}
