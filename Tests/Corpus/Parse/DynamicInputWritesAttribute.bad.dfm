// A dynamic input computes a value in an input slot; it has no place in the stack to write from.
// EXPECT DFX3047
DynamicInput(Name="Corpus/DI_WritesAttribute", Root="Plugin.DreamFX")
{
    Settings = {
        Usage  = DynamicInput;
        Output = float;
    }

    Body = {
        Particles.SpriteRotation = 0.0;
        return Engine.Time;
    }
}
