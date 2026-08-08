// Usage names one of the six stacks (L1) and nothing else; an unknown one has to list them.
// EXPECT DFX3038
Module(Name="Corpus/M_BadUsage", Root="Plugin.DreamFX")
{
    Settings = {
        Usage = ParticleTick;
    }

    Inputs = {
        float Rate = 1.0;
    }

    Body = {
        Particles.SpriteRotation += Rate * Engine.DeltaTime;
    }
}
