// A module input default is stored on the asset, so it cannot reference anything outside the module.
// EXPECT DFX3044
Module(Name="Corpus/M_NonLiteralDefault", Root="Plugin.DreamFX")
{
    Settings = {
        Usage = ParticleUpdate;
    }

    Inputs = {
        float Rate = User.SpinRate;
    }

    Body = {
        Particles.SpriteRotation += Rate * Engine.DeltaTime;
    }
}
