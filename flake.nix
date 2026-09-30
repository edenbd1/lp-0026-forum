{
  description = "Logos Forum — a serverless forum for Logos Basecamp (LP-0026)";

  inputs = {
    # The same builder and dependency pins as the reference forum-sample-app,
    # which is known to load in Basecamp.
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.2.6";
    # Posts and live traffic. v0.2.1 is the first release with a LIDL contract,
    # which the builder needs to generate the typed wrapper.
    delivery_module.url = "github:logos-co/logos-delivery-module/v0.3.0";
    delivery_module.inputs.logos-module-builder.follows = "logos-module-builder";
    # History snapshots.
    storage_module.url = "github:logos-co/logos-storage-module/v3.0.0";
    storage_module.inputs.logos-module-builder.follows = "logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
