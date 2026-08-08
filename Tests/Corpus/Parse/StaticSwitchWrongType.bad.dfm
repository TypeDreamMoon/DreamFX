// R5: a static switch is resolved at compile time, so it has to be something a switch can branch on.
// EXPECT DFX3034
Module(Name="Corpus/M_StaticSwitchWrongType", Root="Plugin.DreamFX")
{
    Settings = {
        Usage = ParticleUpdate;
    }

    Inputs = {
        float Mode = 1.0 [ StaticSwitch ];
    }

    Body = {
        Particles.SpriteRotation += Mode * Engine.DeltaTime;
    }
}
