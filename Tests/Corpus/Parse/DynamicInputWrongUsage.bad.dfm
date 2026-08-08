// A DynamicInput document declaring a stack usage is two statements about what it is that disagree.
// EXPECT DFX3032
DynamicInput(Name="Corpus/DI_WrongUsage", Root="Plugin.DreamFX")
{
    Settings = {
        Usage  = ParticleUpdate;
        Output = float;
    }

    Body = {
        return Engine.Time;
    }
}
