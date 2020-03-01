#include "automaticMeshSizeField.h"
#include "GModel.h"
#include "GRegion.h"
#include "GFace.h"
#include "GEntity.h"
#include "MVertex.h"

#include <chrono>
#include <inttypes.h>

#ifdef HAVE_HXT
extern "C" {
#include "hxt_edge.h"
#include "hxt_curvature.h"
#include "hxt_bbox.h"
#include "hxt_mesh3d.h"
#include "hxt_mesh3d_main.h"
#include "hxt_boundary_recovery.h"
#include "hxt_tetMesh.h"
#include "hxt_tetUtils.h"
#include "hxt_tetFlag.h"
}
#endif

#define TIME

static HXTStatus recoveryFun(HXTMesh* mesh, void* userData) {
  HXT_UNUSED(userData);
  HXT_CHECK( hxt_boundary_recovery(mesh) );
  return HXT_STATUS_OK;
}

HXTStatus Gmsh2HxtLocal(std::vector<GFace *> &faces, HXTMesh *m,
       std::map<MVertex *, int> &v2c,
       std::vector<MVertex *> &c2v);

HXTStatus Gmsh2HxtGlobal(std::vector<GFace *> &faces, HXTMesh *m,
       std::map<MVertex *, int> &v2c,
       std::vector<MVertex *> &c2v);

int counter = 0;

static double meshSize(double x, double y, double z, void *user_data){
  // printf("%d\n", ++counter);
  HXTForest *forest = (HXTForest *) user_data;
  double val = 1.e22;
  HXTStatus s = hxtOctreeSearchOne(forest, x, y, z, &val);
  if (s == HXT_STATUS_OK){
    return val;
  }
  else Msg::Error ("Cannot find point %g %g %g in the octree",x,y,z);
  return val;
}

double automaticMeshSizeField::operator()(double X, double Y, double Z, GEntity *ge) {  
#ifdef HAVE_HXT
  double val = 1.e22;
#if defined(HAVE_HXT) && defined(HAVE_P4EST)
  HXTStatus s = hxtOctreeSearchOne(forest, X, Y, Z, &val);
  if (s == HXT_STATUS_OK){
    return val;
  }
  else Msg::Error ("Cannot find point %g %g %g in the octree",X,Y,Z);
#else
  Msg::Error ("Gmsh has to be compiled with HXT and P4EST for using automaticMeshSizeField");
#endif
  return val;
}

automaticMeshSizeField::~automaticMeshSizeField(){
#if defined(HAVE_HXT) && defined(HAVE_P4EST)
  if (forest)hxtForestDelete(&forest);
  if (forestOptions)hxtForestOptionsDelete(&forestOptions);
#endif
}

void writeNodalCurvature(double *nodalCurvature, int size, const char *filename){

    FILE* f = fopen(filename, "w");
    if(f==NULL){
    printf("Erreur : fileOutput == NULL\n");
      exit(-1);
    }

    for(int i = 0; i < size; ++i){
      fprintf(f, "%f %f %f %f %f %f\n",
            nodalCurvature[6 * i + 0],
            nodalCurvature[6 * i + 1],
            nodalCurvature[6 * i + 2],
            nodalCurvature[6 * i + 3],
            nodalCurvature[6 * i + 4],
            nodalCurvature[6 * i + 5]);
    }

    fclose(f);

}

#if defined(HAVE_HXT) && defined(HAVE_P4EST)

HXTStatus Gmsh2Hxt(std::vector<GRegion *> &regions, HXTMesh *m,
		   std::map<MVertex *, int> &v2c,
		   std::vector<MVertex *> &c2v);

HXTStatus Gmsh2Hxt(std::vector<GFace *> &faces, HXTMesh *m,
       std::map<MVertex *, int> &v2c,
       std::vector<MVertex *> &c2v);

static inline int node2trianglescmp(const void *p0, const void *p1)
{ 
  uint64_t *e0 = (uint64_t*)p0;
  uint64_t *e1 = (uint64_t*)p1;
  
  if (e0[0] < e1[0]) return -1;
  if (e0[0] > e1[0]) return  1;
  return 0;
}

HXTStatus automaticMeshSizeField:: updateHXT(){

  if(!update_needed)
    return HXT_STATUS_OK;
  
  if (forest)        HXT_CHECK(hxtForestDelete(&forest));  
  if (forestOptions) HXT_CHECK(hxtForestOptionsDelete(&forestOptions));

  update_needed = false;

  // Soit on charge un fichier avec l'octree (ma préférence)
  if(!_forestFile.empty()){
    std::cout << "Loading size field file " << _forestFile << std::endl;
    HXT_CHECK( hxtForestLoad(&forest, _forestFile.c_str()) );

  } else{
    // Soit on calcule "en live" l'octree ici
    printf("#regions = %lu \n", GModel::current()->getNumRegions());
    printf("#faces = %lu \n", GModel::current()->getNumFaces());
    
    // std::vector<GRegion*> regions;
    // regions.push_back(*(GModel::current()->firstRegion()));

    // HXTMesh *mesh;
    // HXTContext *context;
    // HXT_CHECK(hxtContextCreate(&context));
    // HXT_CHECK(hxtMeshCreate(context, &mesh));
    // std::map<MVertex *, int> v2c;
    // std::vector<MVertex *> c2v;  
    // Gmsh2Hxt(regions, mesh, v2c, c2v);

    std::vector<GFace*> faces;
    std::set<GFace *, GEntityPtrLessThan>::iterator it = GModel::current()->firstFace();

    while(it != GModel::current()->lastFace()){
      faces.push_back(*it);
      ++it;
    }

    // Create HXT mesh structure
    HXTMesh *mesh;
    HXTContext *context;
    HXT_CHECK(hxtContextCreate(&context));
    HXT_CHECK(hxtMeshCreate(context, &mesh));
    std::map<MVertex *, int> v2c;
    std::vector<MVertex *> c2v;  
    Gmsh2Hxt(faces, mesh, v2c, c2v);

    printf("%lu elements in the mesh \n", mesh->triangles.num);

    double *nodalCurvature;

    // Courbure en chaque noeud (global) 
    HXT_CHECK(hxtMalloc(&nodalCurvature,6*mesh->vertices.num*sizeof(double))); 
    for(uint64_t i = 0; i < 6*mesh->vertices.num; ++i) nodalCurvature[i] = NAN; 

    // Create HXT mesh structure for each GFace
    std::vector<HXTMesh *> faceMeshes;
    
    for(size_t i = 0; i < faces.size(); ++i){
      HXTMesh *meshFace;
      HXT_CHECK(hxtMeshCreate(context, &meshFace));
      std::vector<GFace*> oneFace;
      oneFace.push_back(faces[i]);
      std::map<MVertex *, int> v2cLoc;
      std::vector<MVertex *> c2vLoc;  
      Gmsh2HxtLocal(oneFace, meshFace, v2cLoc, c2vLoc);
      faceMeshes.push_back(meshFace);
    }
    
    std::map<MVertex *, int> v2c2;
    std::vector<MVertex *> c2v2;  
    Gmsh2HxtGlobal(faces, NULL, v2c2, c2v2);

    size_t nVertices = 0;

    // Boucle sur les faces
    for(size_t j = 0; j < faces.size(); j++) {

      HXTMesh *meshFace = faceMeshes[j];

      // Compute curvature of the face
      double   *curvatureCrossfieldFace;
      double   *nodalCurvatureFace;
      HXTEdges *edgesFace;
      HXT_CHECK(hxtEdgesCreate(meshFace,&edgesFace));
      int debug = true;
      HXT_CHECK(hxtCurvatureRusinkiewicz(meshFace,&nodalCurvatureFace,&curvatureCrossfieldFace,edgesFace,debug));

      if(debug){
        for(uint64_t i = 0; i < meshFace->vertices.num*6; ++i){
          if(isnan(nodalCurvatureFace[i])){
            printf("Après le calcul de courbure c[%lu] = %f\n", i, nodalCurvatureFace[i]);
            // printf("nVertices = %lu et *6 = %lu\n", meshFace->vertices.num, meshFace->vertices.num*6);
          }
        }

        for(uint64_t i = 0; i < meshFace->vertices.num; ++i){
          double norme;
          double *v1 = nodalCurvatureFace + 6*i;
          hxtNorm2V3(v1, &norme);
          if(norme > 100){
            printf("%u vertices on face %lu \n", meshFace->vertices.num, j);
            printf(" v1 = %f \t %f \t %f - norme = %f\n", 
              nodalCurvatureFace[6*i+0],
              nodalCurvatureFace[6*i+1],
              nodalCurvatureFace[6*i+2],
              norme);
          }
          double *v2 = nodalCurvatureFace + 6*i + 3;
          hxtNorm2V3(v2, &norme);
          if(norme > 100){
            printf("%u vertices on face %lu \n", meshFace->vertices.num, j);
            printf(" v2 = %f \t %f \t %f - norme = %f\n\n", 
              nodalCurvatureFace[6*i+3],
              nodalCurvatureFace[6*i+4],
              nodalCurvatureFace[6*i+5],
              norme);
          }
        }
      }

      // Replace curvature vectors on the face in global nodalCurvature structure
      for (uint64_t i = 0; i<meshFace->vertices.num; ++i){
        uint64_t nodeGlobal = v2c[ c2v2[nVertices+i] ];
        nodalCurvature[6 * nodeGlobal + 0] = nodalCurvatureFace[6 * i + 0];
        nodalCurvature[6 * nodeGlobal + 1] = nodalCurvatureFace[6 * i + 1];
        nodalCurvature[6 * nodeGlobal + 2] = nodalCurvatureFace[6 * i + 2];
        nodalCurvature[6 * nodeGlobal + 3] = nodalCurvatureFace[6 * i + 3];
        nodalCurvature[6 * nodeGlobal + 4] = nodalCurvatureFace[6 * i + 4];
        nodalCurvature[6 * nodeGlobal + 5] = nodalCurvatureFace[6 * i + 5];        
      }

      nVertices += meshFace->vertices.num;

      HXT_CHECK(hxtEdgesDelete(&edgesFace)       );
      HXT_CHECK(hxtFree(&curvatureCrossfieldFace));
      HXT_CHECK(hxtFree(&nodalCurvatureFace)     );
    } // for faces.size()
    
    // writeNodalCurvature(nodalCurvature, mesh->vertices.num, "separe.txt");

    // Compute normals
    double   *nodeNormals;
    HXT_CHECK(hxtCurvatureNormals(mesh, &nodeNormals, 0));

    // HXT_CHECK(hxtEdgesCreate(mesh,&edges));
    // HXT_CHECK(hxtCurvatureRusinkiewicz(mesh,&nodalCurvature2,&curvatureCrossfield,edges,1));
    // HXT_CHECK(hxtEdgesDelete(&edges));
    // HXT_CHECK(hxtFree(&curvatureCrossfield));

    // writeNodalCurvature(nodalCurvature2, mesh->vertices.num, "nonSepare.txt");

    // Compute rtree
    RTree<uint64_t, double, 3> triRTree;
    HXTBbox bbox_triangle;

    for(uint64_t i = 0; i < mesh->triangles.num; ++i){
      hxtBboxInit(&bbox_triangle);
      // Noeuds
      for(uint64_t j = 0; j < 3; ++j){
        double coord[3];
        uint32_t node = mesh->triangles.node[3*i+j];
        // Coordonnees
        for(uint32_t k = 0; k < 3; ++k){ coord[k] = mesh->vertices.coord[(size_t) 4*node+k]; }
        hxtBboxAddOne(&bbox_triangle, coord);
      }
      triRTree.Insert(bbox_triangle.min,bbox_triangle.max, i);
    }

    // Compute bbox of the mesh
    double            bbox_vertices[6];
    HXTBbox           bbox_mesh;
    hxtBboxInit(&bbox_mesh);
    // Ajout de tous les points du maillage à la bounding box
    hxtBboxAdd(&bbox_mesh, mesh->vertices.coord, mesh->vertices.num);
    for(int i = 0; i < 3; ++i){
      bbox_vertices[i  ] = bbox_mesh.min[i];
      bbox_vertices[i+3] = bbox_mesh.max[i];
    }

    if(_hbulk < 0 || _hmin < 0){
      double L = -1.0;
      for(int i = 0; i < 3; ++i)
        L = fmax(L, bbox_mesh.max[i] - bbox_mesh.min[i] );
      printf("Taille de la boîte = %f\n", L);
      _hbulk < 0 ? _hbulk = L/10.   : _hbulk;
      _hmin  < 0 ? _hmin  = L/1000. : _hmin;
      printf("hbulk = %f\n", _hbulk);
      printf("hmin  = %f\n", _hmin);

    }

    if(_hmax < 0)
      _hmax = _hbulk;

    HXT_CHECK(hxtForestOptionsCreate(&forestOptions));
    forestOptions->nodePerTwoPi = _nPointsPerCircle;
    forestOptions->nodePerGap = _nPointsPerGap;
    forestOptions->hmax = _hmax;  
    forestOptions->hmin = _hmin;  
    forestOptions->hbulk = _hbulk;  
    forestOptions->gradMax = _gradientMax;  
    forestOptions->nRefine = _nRefine;
    forestOptions->bbox = bbox_vertices;
    forestOptions->nodalCurvature = nodalCurvature;
    forestOptions->nodeNormals = nodeNormals;
    forestOptions->triRTree = &triRTree;
    forestOptions->mesh = mesh;
    forestOptions->sizeFunction = NULL;
    forestOptions->filename = "octreeExport.pos";

    HXT_CHECK(hxtForestCreate(0, NULL, &forest, NULL, forestOptions));

    if (forestOptions->nodePerTwoPi > 0){
      // Refine the octree based on curvature of the surface mesh
      HXT_CHECK(hxtForestRefine(forest));

      // Smooth the size gradient
      if(_smoothing){ HXT_CHECK( hxtForestSizeSmoothing(forest)); }
    }
    
    if(_gaps){
      printf("======= Detecting gaps ======= \n");
      HXT_CHECK(hxtOctreeSurfacesProches(forest));

      if(_smoothing){ HXT_CHECK( hxtForestSizeSmoothing(forest)); }
    }

    double elemEstimation;
    HXT_CHECK(hxtOctreeElementEstimation(forest, &elemEstimation));
    printf("Estimation du nombre d'éléments : %f \n", elemEstimation);

    // Export size field in .pos file
    HXT_CHECK(hxtForestExport(forest));  

    // Maillage 2D ?
    // printf("Meshing 2D\n");
    // GModel::current()->deleteMesh();
    // GModel::current()->getFields()->setBackgroundField(this);
    // GModel::current()->mesh(2);

    // Maillage 3D
    // if(_tetMesh){
    //   printf("Meshing 3D\n");
    //   const char* input = NULL;
    //   const char* output = "tetMesh.msh";

    //   printf("Nombre de threads OpenMP : %d\n", omp_get_max_threads());

    //   HXTTetMeshOptions options = {
    //     .defaultThreads=8,
    //     .delaunayThreads=8,
    //     .improveThreads=8,
    //     .reproducible=0,
    //     .verbosity=2,
    //     .stat=true,
    //     .refine=1,
    //     .optimize=1, 
    //     .qualityMin=0.35,
    //     .qualityFun=NULL,
    //     .qualityData=NULL, 
    //     .meshSizeFun=meshSize,
    //     .meshSizeData=forest, 
    //     .recoveryFun=recoveryFun,
    //     .recoveryData=NULL
    //   };

    //   if(options.verbosity)
    //   {
    //     const char* T = "true";
    //     const char* F = "false";
    //     printf("\ninput:             %s\noutput:            %s\n"
    //              "Number of threads: %d\nDelaunay threads:  %d\nOptim. threads:    %d\n"
    //              "reproducible:      %s\nverbosity level:   %d\nshow statistics:   %s\n"
    //              "refine:            %s\noptimize :         %s\nmin aspect ratio:  %.3f\n\n",
    //            input?input:"-", output?output:"-", options.defaultThreads<=0?omp_get_max_threads():options.defaultThreads,
    //            options.delaunayThreads, options.improveThreads,
    //            options.reproducible?T:F, (int) options.verbosity, options.stat?T:F, options.refine?T:F, options.optimize?T:F, options.qualityMin);
    //   }

    //   HXT_CHECK(hxtTetMesh(mesh, &options));

    //   for(uint64_t i = 0; i < mesh->tetrahedra.num; ++i){
    //     if(mesh->tetrahedra.colors[i] == UINT16_MAX){
    //       setDeletedFlag(mesh, i);
    //     }
    //   }

    //   HXT_CHECK( hxtRemoveDeleted(mesh) );

    //   HXT_CHECK(hxtMeshWriteGmsh(mesh, output));
    // }

    if(nodalCurvature) HXT_CHECK(hxtFree(&nodalCurvature));
    if(nodeNormals)    HXT_CHECK(hxtFree(&nodeNormals));
    HXT_CHECK(hxtMeshDelete(&mesh));
    HXT_CHECK(hxtContextDelete(&context));
  }

  return HXT_STATUS_OK;

#endif //ifdef HAVE_P4EST
}

#endif

void automaticMeshSizeField::update(){
#if defined(HAVE_HXT) && defined(HAVE_P4EST)
  HXTStatus s = updateHXT();
  if (s != HXT_STATUS_OK)Msg::Error ("Something went wrong when computing the octree");
#else
  Msg::Error ("Gmsh has to be compiled with HXT and P4EST to use automaticMeshSizeField");
#endif
};

static HXTStatus getAllEdgesOfAllFaces(std::vector<GFace *> &faces, HXTMesh *m,
                                       std::vector<GEdge *> &allEdges)
{
  if(m) {
    m->brep.numSurfaces = faces.size();
    HXT_CHECK(hxtAlignedMalloc(&m->brep.numCurvesPerSurface,
                               m->brep.numSurfaces * sizeof(uint32_t)));
  }
  uint32_t to_alloc = 0;

  std::set<GEdge *, GEntityPtrLessThan> allEdgesSet;
  for(std::size_t i = 0; i < faces.size(); i++) {
    std::vector<GEdge *> const &f = faces[i]->edges();
    std::vector<GEdge *> const &f_e = faces[i]->embeddedEdges();
    if(m) {
      m->brep.numCurvesPerSurface[i] = f.size() + f_e.size();
      to_alloc += m->brep.numCurvesPerSurface[i];
    }
    allEdgesSet.insert(f.begin(), f.end());
    allEdgesSet.insert(f_e.begin(), f_e.end());
  }
  allEdges.insert(allEdges.begin(), allEdgesSet.begin(), allEdgesSet.end());

  if(!m) return HXT_STATUS_OK;

  HXT_CHECK(
    hxtAlignedMalloc(&m->brep.curvesPerSurface, to_alloc * sizeof(uint32_t)));

  uint32_t counter = 0;
  for(std::size_t i = 0; i < faces.size(); i++) {
    std::vector<GEdge *> const &f = faces[i]->edges();
    std::vector<GEdge *> const &f_e = faces[i]->embeddedEdges();
    for(size_t j = 0; j < f.size(); j++)
      m->brep.curvesPerSurface[counter++] = f[j]->tag();
    for(size_t j = 0; j < f_e.size(); j++)
      m->brep.curvesPerSurface[counter++] = f_e[j]->tag();
  }
  return HXT_STATUS_OK;
}

HXTStatus Gmsh2HxtLocal(std::vector<GFace *> &faces, HXTMesh *m,
       std::map<MVertex *, int> &v2c,
       std::vector<MVertex *> &c2v)
{
  std::vector<GEdge *> edges;
  HXT_CHECK(getAllEdgesOfAllFaces(faces, m, edges));
  std::set<MVertex *> all;


  uint64_t ntri = 0;
  uint64_t nedg = 0;

  for(size_t j = 0; j < edges.size(); j++) {
    GEdge *ge = edges[j];
    nedg += ge->lines.size();
    for(size_t i = 0; i < ge->lines.size(); i++) {
      all.insert(ge->lines[i]->getVertex(0));
      all.insert(ge->lines[i]->getVertex(1));
    }
  }

  for(size_t j = 0; j < faces.size(); j++) {
    GFace *gf = faces[j];
    ntri += gf->triangles.size();
    for(size_t i = 0; i < gf->triangles.size(); i++) {
      all.insert(gf->triangles[i]->getVertex(0));
      all.insert(gf->triangles[i]->getVertex(1));
      all.insert(gf->triangles[i]->getVertex(2));
    }
  }

  //  printf("%d vertices %d triangles\n",all.size(),ntri);

  m->vertices.num = m->vertices.size = all.size();
  HXT_CHECK(
    hxtAlignedMalloc(&m->vertices.coord, 4 * m->vertices.num * sizeof(double)));

  size_t count = 0;
  c2v.resize(all.size());
  for(std::set<MVertex *>::iterator it = all.begin(); it != all.end(); it++) {
    m->vertices.coord[4 * count + 0] = (*it)->x();
    m->vertices.coord[4 * count + 1] = (*it)->y();
    m->vertices.coord[4 * count + 2] = (*it)->z();
    m->vertices.coord[4 * count + 3] = 0.0;
    v2c[*it] = count;
    c2v[count++] = *it;
  }
  all.clear();

  m->lines.num = m->lines.size = nedg;
  uint64_t index = 0;

  HXT_CHECK(
    hxtAlignedMalloc(&m->lines.node, (m->lines.num) * 2 * sizeof(uint32_t)));
  HXT_CHECK(
    hxtAlignedMalloc(&m->lines.colors, (m->lines.num) * sizeof(uint16_t)));

  for(size_t j = 0; j < edges.size(); j++) {
    GEdge *ge = edges[j];
    for(size_t i = 0; i < ge->lines.size(); i++) {
      m->lines.node[2 * index + 0] = v2c[ge->lines[i]->getVertex(0)];
      m->lines.node[2 * index + 1] = v2c[ge->lines[i]->getVertex(1)];
      m->lines.colors[index] = ge->tag();
      index++;
    }
  }

  m->triangles.num = m->triangles.size = ntri;
  HXT_CHECK(hxtAlignedMalloc(&m->triangles.node,
                             (m->triangles.num) * 3 * sizeof(uint32_t)));
  HXT_CHECK(hxtAlignedMalloc(&m->triangles.colors,
                             (m->triangles.num) * sizeof(uint16_t)));

  index = 0;
  for(size_t j = 0; j < faces.size(); j++) {
    GFace *gf = faces[j];
    for(size_t i = 0; i < gf->triangles.size(); i++) {
      m->triangles.node[3 * index + 0] = v2c[gf->triangles[i]->getVertex(0)];
      m->triangles.node[3 * index + 1] = v2c[gf->triangles[i]->getVertex(1)];
      m->triangles.node[3 * index + 2] = v2c[gf->triangles[i]->getVertex(2)];
      m->triangles.colors[index] = gf->tag();
      index++;
    }
  }
  return HXT_STATUS_OK;
}

HXTStatus Gmsh2HxtGlobal(std::vector<GFace *> &faces, HXTMesh *m,
       std::map<MVertex *, int> &v2c,
       std::vector<MVertex *> &c2v)
{
  std::vector<GEdge *> edges;
  HXT_CHECK(getAllEdgesOfAllFaces(faces, m, edges));
  std::set<MVertex *> all;

  uint64_t cumsum = 0;
  uint64_t cumsumNoEdges = 0;
  for(size_t j = 0; j < edges.size(); j++) {
    GEdge *ge = edges[j];
    cumsum += ge->lines.size();
    for(size_t i = 0; i < ge->lines.size(); i++) {
      all.insert(ge->lines[i]->getVertex(0));
      all.insert(ge->lines[i]->getVertex(1));
    }
  }

  for(size_t j = 0; j < faces.size(); j++) {
    GFace *gf = faces[j];
    cumsum += gf->triangles.size();
    cumsumNoEdges += gf->triangles.size();
    for(size_t i = 0; i < gf->triangles.size(); i++) {
      all.insert(gf->triangles[i]->getVertex(0));
      all.insert(gf->triangles[i]->getVertex(1));
      all.insert(gf->triangles[i]->getVertex(2));
    }
  }
  // c2v.resize(all.size());
  c2v.resize(cumsum);
  // printf("all.size() = %lu\n", all.size());
  // printf("cumsum incluant les edges = %lu\n", cumsum);
  // printf("cumsum excluant les edges = %lu\n", cumsumNoEdges);
  all.clear();
  
  cumsum = 0;
  size_t count_c2v2 = 0;
  for(size_t j = 0; j < faces.size(); ++j){
    // all propre à la face
    GFace *gf = faces[j];
    cumsum += gf->triangles.size();
    for(size_t i = 0; i < gf->triangles.size(); i++) {
      all.insert(gf->triangles[i]->getVertex(0));
      all.insert(gf->triangles[i]->getVertex(1));
      all.insert(gf->triangles[i]->getVertex(2));
    }
    // printf("sub all.size() = %lu\n", all.size());
    // printf("cumsum = %lu\n", cumsum);

    size_t count = 0;
    for(std::set<MVertex *>::iterator it = all.begin(); it != all.end(); it++) {
      v2c[*it] = count++;
      c2v[count_c2v2++] = *it;
    }
    all.clear();
  }

  return HXT_STATUS_OK;
}
