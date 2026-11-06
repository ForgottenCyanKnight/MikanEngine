// Visibility ABI shared by raster packing and hardware/software ray queries.
// instancePlusOne==0 is sky; bary is (weight of vertex 1, weight of vertex 2).
struct PrimaryHit {uint instancePlusOne;uint geometry;uint primitive;float hitT;vec2 bary;};
layout(std430,binding=34) buffer PrimaryHits {PrimaryHit primaryHits[];};
layout(std430,binding=61) readonly buffer RasterVisibilityWords {uint visibilityWords[];};
