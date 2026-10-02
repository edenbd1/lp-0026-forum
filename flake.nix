{
  description = "Logos Forum: a serverless forum for Logos Basecamp (LP-0026)";

  # The Logos binary cache, for the dependency modules' prebuilt artifacts.
  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [ "public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU=" ];
  };

  inputs = {
    # The builder delivery_module 0.3.0 and chat_module 0.3.0 are built with.
    # From 0.3 on it also targets x86_64-windows (a mingw cross build) and reads
    # versioned entries ({ name, version }) in metadata.json's dependencies.
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    # Posts and live traffic.
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
