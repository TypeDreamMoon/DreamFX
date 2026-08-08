// An empty body generates a module that occupies a stack slot and does nothing.
// EXPECT DFX3036
Module(Name="Corpus/M_EmptyBody", Root="Plugin.DreamFX")
{
    Settings = {
        Usage = ParticleUpdate;
    }

    Inputs = {
        float Rate = 1.0;
    }

    Body = {
    }
}
