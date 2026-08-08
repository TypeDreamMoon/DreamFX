// R5 again, from the other side: a switch resolved at compile time cannot take a runtime value.
// EXPECT DFX3035
Module(Name="Corpus/M_StaticSwitchNonConstant", Root="Plugin.DreamFX")
{
    Settings = {
        Usage = ParticleUpdate;
    }

    Inputs = {
        bool bEnabled = User.SomeFlag [ StaticSwitch ];
    }

    Body = {
        Particles.SpriteRotation += Engine.DeltaTime;
    }
}
