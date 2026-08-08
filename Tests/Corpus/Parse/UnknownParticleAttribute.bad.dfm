// plan-v2 W1: a custom attribute has no type the engine knows, and guessing one would wire a pin of
// the wrong width. The message asks for the same type annotation a .dfs uses.
// EXPECT DFX3046
Module(Name="Corpus/M_UnknownAttribute", Root="Plugin.DreamFX")
{
    Settings = {
        Usage = ParticleUpdate;
    }

    Inputs = {
        float Rate = 1.0;
    }

    Body = {
        Particles.Moon.SpinPhase += Rate * Engine.DeltaTime;
    }
}
