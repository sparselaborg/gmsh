SetFactory("OpenCASCADE");

Mesh.CharacteristicLengthMax = 0.4;

R = 2;
Block(1) = {0,0,0, R,R,R};
pts() = Unique(Boundary{Boundary{Boundary{Volume{1};}}});

Characteristic Length{pts(0)} = 0.01;

SyncModel;
Periodic Surface{2} = {1} Translate{R,0,0};
