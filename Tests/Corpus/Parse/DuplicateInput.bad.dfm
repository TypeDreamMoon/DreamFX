// Two inputs with one name would collide on the same Module. parameter.
// EXPECT DFX3033
Module(Name="Corpus/M_DuplicateInput", Root="Plugin.DreamFX")
{
    Settings = {
        Usage = ParticleUpdate;
    }

    Inputs = {
        float Rate = 1.0;
        float Rate = 2.0;
    }

    Body = {
        Particles.SpriteRotation += Rate * Engine.DeltaTime;
    }
}
