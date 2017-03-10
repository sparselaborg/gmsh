// Copyright (C) 2013 ULg-UCL
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, and/or sell copies of the
// Software, and to permit persons to whom the Software is furnished
// to do so, provided that the above copyright notice(s) and this
// permission notice appear in all copies of the Software and that
// both the above copyright notice(s) and this permission notice
// appear in supporting documentation.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE BE LIABLE FOR
// ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY
// DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
// WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
// ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
// OF THIS SOFTWARE.
//
// Please report all bugs and problems to the public mailing list
// <gmsh@onelab.info>.
//
// Contributors: Thomas Toulorge, Jonathan Lambrechts

#include <cstdio>
#include <sstream>
#include <fstream>
#include <string>
#include <cmath>
#include "OptHomFastCurving.h"
#include "GmshConfig.h"
#include "GModel.h"
#include "Gmsh.h"
#include "MLine.h"
#include "MTriangle.h"
#include "MQuadrangle.h"
#include "MTetrahedron.h"
#include "MHexahedron.h"
#include "MPrism.h"
#include "MEdge.h"
#include "MFace.h"
#include "OS.h"
#include "SVector3.h"
#include "BasisFactory.h"
#include "MetaEl.h"
#include "qualityMeasuresJacobian.h"



namespace {



typedef std::map<MEdge, std::vector<MElement*>, Less_Edge> MEdgeVecMEltMap;
typedef std::map<MFace, std::vector<MElement*>, Less_Face> MFaceVecMEltMap;



// Compute edge -> element connectivity (for 2D elements)
void calcEdge2Elements(GEntity *entity, MEdgeVecMEltMap &ed2el)
{
  for (size_t iEl = 0; iEl < entity->getNumMeshElements(); iEl++) {
    MElement *elt = entity->getMeshElement(iEl);
    if (elt->getDim() == 2)
      for (int iEdge = 0; iEdge < elt->getNumEdges(); iEdge++) {
        ed2el[elt->getEdge(iEdge)].push_back(elt);
      }
  }
}



// Compute face -> element connectivity (for 3D elements)
void calcFace2Elements(GEntity *entity, MFaceVecMEltMap &face2el)
{
  for (size_t iEl = 0; iEl < entity->getNumMeshElements(); iEl++) {
    MElement *elt = entity->getMeshElement(iEl);
    if (elt->getDim() == 3)
      for (int iFace = 0; iFace < elt->getNumFaces(); iFace++)
        face2el[elt->getFace(iFace)].push_back(elt);
  }
}



// Get the face index of an element given a MFace
inline int getElementFace(const MFace &face, MElement *el)
{
  for (int iFace = 0; iFace < el->getNumFaces(); iFace++)
    if (el->getFace(iFace) == face) return iFace;
  return -1;
}



// Get the edge index of an element given a MEdge
inline int getElementEdge(const MEdge &edge, MElement *el)
{
  for (int iEdge = 0; iEdge < el->getNumEdges(); iEdge++)
    if (el->getEdge(iEdge) == edge) return iEdge;
  return -1;
}



void makeStraight(MElement *el, const std::set<MVertex*> &movedVert)
{
  const nodalBasis *nb = el->getFunctionSpace();
  const fullMatrix<double> &pts = nb->points;

  SPoint3 p;

  for(int iPt = el->getNumPrimaryVertices(); iPt < el->getNumVertices(); ++iPt) {
    MVertex *vert = el->getVertex(iPt);
    if (movedVert.find(vert) == movedVert.end()) {
      el->primaryPnt(pts(iPt,0),pts(iPt,1),pts(iPt,2),p);
      vert->setXYZ(p.x(),p.y(),p.z());
    }
  }
}



inline void insertIfCurved(MElement *el, std::list<MElement*> &bndEl)
{
  static const double curvedTol = 1.e-3;                                              // Tolerance to consider element as curved

  const double normalDispCurved = curvedTol*el->getInnerRadius();                     // Threshold in normal displacement to consider element as curved
  const int dim = el->getDim();

  // Compute unit normal to straight edge/face
  SVector3 normal = (dim == 1) ? el->getEdge(0).normal() :
                                 el->getFace(0).normal();

  // Get functional spaces
  const nodalBasis *lagBasis = el->getFunctionSpace();
  const fullMatrix<double> &uvw = lagBasis->points;
  const int &nV = uvw.size1();
  const nodalBasis *lagBasis1 = el->getFunctionSpace(1);
  const int &nV1 = lagBasis1->points.size1();

  // Get first-order vertices
  std::vector<SPoint3> xyz1(nV1);
  for (int iV = 0; iV < nV1; ++iV) xyz1[iV] = el->getVertex(iV)->point();

  // Check normal displacement at each HO vertex
  for (int iV = nV1; iV < nV; ++iV) {                                                 // Loop over HO nodes
    double f[256];
    lagBasis1->f(uvw(iV, 0), (dim > 1) ? uvw(iV, 1) : 0., 0., f);
    SPoint3 xyzS(0.,0.,0.);
    for (int iSF = 0; iSF < nV1; ++iSF) xyzS += xyz1[iSF]*f[iSF];                     // Compute location of node in straight element
    const SVector3 vec(xyzS, el->getVertex(iV)->point());
    const double normalDisp = fabs(dot(vec, normal));                                 // Normal component of displacement
    if (normalDisp > normalDispCurved) {
      bndEl.push_back(el);
      break;
    }
  }
}



void getOppositeEdge(MElement *el, const MEdge &elBaseEd, MEdge &elTopEd,
                     double &edLenMin, double &edLenMax)
{
  static const double BIG = 1e300;

  // Find base face
  const int iElBaseEd = getElementEdge(elBaseEd, el);
  edLenMin = BIG;
  edLenMax = -BIG;
  MEdge elMaxEd;

  // Look for largest edge except base one
  for (int iElEd = 0; iElEd < el->getNumEdges(); iElEd++) {
    if (iElEd != iElBaseEd) {
      MEdge edTest = el->getEdge(iElEd);
      double edLenTest = edTest.length();
      if (edLenTest < edLenMin) edLenMin = edLenTest;
      if (edLenTest > edLenMax) {
        edLenMax = edLenTest;
        elMaxEd = edTest;
      }
    }
  }

  // Build top edge from max edge (with right orientation)
  bool sameOrient = false;
  if (el->getType() == TYPE_TRI) {                                    // Triangle: look for common vertex
    sameOrient = (elBaseEd.getVertex(0) == elMaxEd.getVertex(0));
  }
  else {                                                              // Quad: look for edge between base and top vert.
    MEdge sideEdTest(elBaseEd.getVertex(0), elMaxEd.getVertex(0));
    for (int iElEd = 0; iElEd < el->getNumEdges(); iElEd++)
      if (el->getEdge(iElEd) == sideEdTest) {
        sameOrient = true;
        break;
      }
  }
  if (sameOrient) elTopEd = elMaxEd;
  else elTopEd = MEdge(elMaxEd.getVertex(1), elMaxEd.getVertex(0));
}



void getColumnQuad(MEdgeVecMEltMap &ed2el, const FastCurvingParameters &p,
                   MEdge &elBaseEd,  std::vector<MElement*> &blob)
{
  const double maxDP = std::cos(p.maxAngle);

  MElement *el = 0;

  for (int iLayer = 0; iLayer < p.maxNumLayers; iLayer++) {
    std::vector<MElement*> newElts = ed2el[elBaseEd];
    el = (newElts[0] == el) ? newElts[1] : newElts[0];
    if (el->getType() != TYPE_QUA) break;
    MEdge elTopEd;
    double edLenMin, edLenMax;
    getOppositeEdge(el, elBaseEd, elTopEd, edLenMin, edLenMax);

    if (edLenMin > edLenMax*p.maxRho) break;
    const double dp = dot(elBaseEd.normal(), elTopEd.normal());
    if (std::abs(dp) < maxDP) break;

    blob.push_back(el);
    elBaseEd = elTopEd;
  }
}



void getColumnTri(MEdgeVecMEltMap &ed2el, const FastCurvingParameters &p,
                   MEdge &elBaseEd, std::vector<MElement*> &blob)
{
  const double maxDP = std::cos(p.maxAngle);

  MElement *el0 = 0, *el1 = 0;

  for (int iLayer = 0; iLayer < p.maxNumLayers; iLayer++) {
    std::vector<MElement*> newElts0 = ed2el[elBaseEd];
    el0 = (newElts0[0] == el1) ? newElts0[1] : newElts0[0];
    if (el0->getType() != TYPE_TRI) break;
    MEdge elMidEd;
    double edLenMin0, edLenMax0;
    getOppositeEdge(el0, elBaseEd, elMidEd, edLenMin0, edLenMax0);

    std::vector<MElement*> newElts1 = ed2el[elMidEd];
    el1 = (newElts1[0] == el0) ? newElts1[1] : newElts1[0];
    if (el1->getType() != TYPE_TRI) break;
    MEdge elTopEd;
    double edLenMin1, edLenMax1;
    getOppositeEdge(el1, elMidEd, elTopEd, edLenMin1, edLenMax1);

    const double edLenMin = std::min(edLenMin0, edLenMin1);
    const double edLenMax = std::max(edLenMax0, edLenMax1);
    if (edLenMin > edLenMax*p.maxRho) break;
    const double dp = dot(elBaseEd.normal(), elTopEd.normal());
    if (std::abs(dp) < maxDP) break;

    blob.push_back(el0);
    blob.push_back(el1);
    elBaseEd = elTopEd;
  }
}



bool getColumn2D(MEdgeVecMEltMap &ed2el, const FastCurvingParameters &p,
                 const MEdge &baseEd, std::vector<MVertex*> &baseVert,
                 std::vector<MVertex*> &topPrimVert,
                 std::vector<MElement*> &blob)
{
  // Get first element and base vertices
  std::vector<MElement*> firstElts = ed2el[baseEd];
  MElement *el = firstElts[0];
  const int iFirstElEd = getElementEdge(baseEd, el);
  el->getEdgeVertices(iFirstElEd, baseVert);
  MEdge elBaseEd(baseVert[0], baseVert[1]);

  // Sweep column upwards by choosing largest edges in each element
  if (el->getType() == TYPE_TRI) getColumnTri(ed2el, p, elBaseEd, blob);
  else getColumnQuad(ed2el, p, elBaseEd, blob);

  topPrimVert.resize(2);
  topPrimVert[0] = elBaseEd.getVertex(0);
  topPrimVert[1] = elBaseEd.getVertex(1);
  return true;
}



void getOppositeFacePrism(MElement *el, const MFace &elBaseFace,
                          MFace &elTopFace, double &faceSurfMin,
                          double &faceSurfMax)
{
  // Find base face
  int iElBaseFace = -1, iDum;
  el->getFaceInfo(elBaseFace, iElBaseFace, iDum, iDum);

  // Check side edges to find opposite vertices
  int edCheck[3] = {2, 4, 5};
  std::vector<MVertex*> topVert(3);
  for (int iEd = 0; iEd < 3; iEd++) {
    MEdge ed = el->getEdge(edCheck[iEd]);
    for (int iV = 0; iV < 3; iV++) {
      if (elBaseFace.getVertex(iV) == ed.getVertex(0))
        topVert[iV] = ed.getVertex(1);
      else if (elBaseFace.getVertex(iV) == ed.getVertex(1))
        topVert[iV] = ed.getVertex(0);
    }
  }
  elTopFace = MFace(topVert);

  // Compute min. (side faces) and max. (top face) face areas
  faceSurfMax = elTopFace.area();
  MFace sideFace0 = el->getFace(2);
  faceSurfMin = sideFace0.area();
  MFace sideFace1 = el->getFace(3);
  faceSurfMin = std::min(faceSurfMin, sideFace1.area());
  MFace sideFace2 = el->getFace(4);
  faceSurfMin = std::min(faceSurfMin, sideFace2.area());
}



void getOppositeFaceHex(MElement *el, const MFace &elBaseFace, MFace &elTopFace,
                        double &faceSurfMin, double &faceSurfMax)
{
  // Find base face
  int iElBaseFace = -1, iDum;
  el->getFaceInfo(elBaseFace, iElBaseFace, iDum, iDum);

  // Determine side edges
  int sideEd[4], sideFace[4];
  if ((iElBaseFace == 0) || (iElBaseFace == 5)) {
    sideEd[0] = 2; sideEd[1] = 4; sideEd[2] = 6; sideEd[3] = 7;
    sideFace[0] = 1; sideFace[1] = 2; sideFace[2] = 3; sideFace[3] = 4;
  }
  else if ((iElBaseFace == 1) || (iElBaseFace == 4)) {
    sideEd[0] = 1; sideEd[1] = 3; sideEd[2] = 10; sideEd[3] = 9;
    sideFace[0] = 0; sideFace[1] = 2; sideFace[2] = 3; sideFace[3] = 5;
  }
  else if ((iElBaseFace == 2) || (iElBaseFace == 3)) {
    sideEd[0] = 0; sideEd[1] = 5; sideEd[2] = 11; sideEd[3] = 8;
    sideFace[0] = 0; sideFace[1] = 1; sideFace[2] = 4; sideFace[3] = 5;
  }

  // Check side edges to find opposite vertices
  std::vector<MVertex*> topVert(4);
  for (int iEd = 0; iEd < 4; iEd++) {
    MEdge ed = el->getEdge(sideEd[iEd]);
    for (int iV = 0; iV < 4; iV++) {
      if (elBaseFace.getVertex(iV) == ed.getVertex(0))
        topVert[iV] = ed.getVertex(1);
      else if (elBaseFace.getVertex(iV) == ed.getVertex(1))
        topVert[iV] = ed.getVertex(0);
    }
  }
  elTopFace = MFace(topVert);

  // Compute min. (side faces) and max. (top face) face areas
  faceSurfMax = elTopFace.area();
  MFace sideFace0 = el->getFace(sideFace[0]);
  faceSurfMin = sideFace0.area();
  MFace sideFace1 = el->getFace(sideFace[1]);
  faceSurfMin = std::min(faceSurfMin, sideFace1.area());
  MFace sideFace2 = el->getFace(sideFace[2]);
  faceSurfMin = std::min(faceSurfMin, sideFace2.area());
  MFace sideFace3 = el->getFace(sideFace[3]);
  faceSurfMin = std::min(faceSurfMin, sideFace3.area());
}



void getOppositeFaceTet(MElement *el, const MFace &elBaseFace, MFace &elTopFace,
                        double &faceSurfMin, double &faceSurfMax)
{
  static const double BIG = 1e300;

  int iElBaseFace = -1, iDum;
  el->getFaceInfo(elBaseFace, iElBaseFace, iDum, iDum);
  faceSurfMin = BIG;
  faceSurfMax = -BIG;
  MFace elMaxFace;

  // Look for largest face except base one
  for (int iElFace = 0; iElFace < el->getNumFaces(); iElFace++) {
    if (iElFace != iElBaseFace) {
      MFace faceTest = el->getFace(iElFace);
      const double faceSurfTest = faceTest.area();
      if (faceSurfTest < faceSurfMin) faceSurfMin = faceSurfTest;
      if (faceSurfTest > faceSurfMax) {
        faceSurfMax = faceSurfTest;
        elMaxFace = faceTest;
      }
    }
  }

  // Build top face from max face (with right correspondance)
  MVertex *const &v0 = elMaxFace.getVertex(0);
  MVertex *const &v1 = elMaxFace.getVertex(1);
  MVertex *const &v2 = elMaxFace.getVertex(2);
  std::vector<MVertex*> topVert(3);
  if (elBaseFace.getVertex(0) == v0) {
    topVert[0] = v0;
    if (elBaseFace.getVertex(1) == v1) { topVert[1] = v1; topVert[2] = v2; }
    else { topVert[1] = v2; topVert[2] = v1; }
  }
  else if (elBaseFace.getVertex(0) == v1) {
    topVert[0] = v1;
    if (elBaseFace.getVertex(1) == v0) { topVert[1] = v0; topVert[2] = v2; }
    else { topVert[1] = v2; topVert[2] = v0; }
  }
  else {
    topVert[0] = v2;
    if (elBaseFace.getVertex(1) == v0) { topVert[1] = v0; topVert[2] = v1; }
    else { topVert[1] = v1; topVert[2] = v0; }
  }
  elTopFace = MFace(topVert);
}



void getColumnPrismHex(int elType, MFaceVecMEltMap &face2el,
                       const FastCurvingParameters &p, MFace &elBaseFace,
                       std::vector<MElement*> &blob)
{
  const double maxDP = std::cos(p.maxAngle);

  MElement *el = 0;

  for (int iLayer = 0; iLayer < p.maxNumLayers; iLayer++) {
    std::vector<MElement*> newElts = face2el[elBaseFace];
    el = (newElts[0] == el) ? newElts[1] : newElts[0];
    if (el->getType() != elType) break;

    MFace elTopFace;
    double faceSurfMin, faceSurfMax;
    if (el->getType() == TYPE_PRI)
      getOppositeFacePrism(el, elBaseFace, elTopFace, faceSurfMin, faceSurfMax);
    else if (el->getType() == TYPE_HEX)
      getOppositeFaceHex(el, elBaseFace, elTopFace, faceSurfMin, faceSurfMax);
//    else if (el->getType() == TYPE_TET)
//      getOppositeFaceTet(el, elBaseFace, elTopFace, faceSurfMin, faceSurfMax);

    if (faceSurfMin > faceSurfMax*p.maxRho) break;
    const double dp = dot(elBaseFace.normal(), elTopFace.normal());
    if (std::abs(dp) < maxDP) break;

    blob.push_back(el);
    elBaseFace = elTopFace;
  }
}



bool getColumn3D(MFaceVecMEltMap &face2el, const FastCurvingParameters &p,
                 const MFace &baseFace, std::vector<MVertex*> &baseVert,
                 std::vector<MVertex*> &topPrimVert,
                 std::vector<MElement*> &blob)
{
  // Get first element and base vertices
  const int nbBaseFaceVert = baseFace.getNumVertices();
  std::vector<MElement*> firstElts = face2el[baseFace];
  MElement *el = firstElts[0];
  const int iFirstElFace = getElementFace(baseFace, el);
  el->getFaceVertices(iFirstElFace, baseVert);
  MFace elBaseFace(baseVert[0], baseVert[1], baseVert[2],
                   (nbBaseFaceVert == 3) ? 0 : baseVert[3]);

  // Sweep column upwards by choosing largest faces in each element
  if (nbBaseFaceVert == 3) {
    if (el->getType() == TYPE_PRI)                                              // Get BL column of prisms
      getColumnPrismHex(TYPE_PRI, face2el, p, elBaseFace, blob);
//    else if (el->getType() == TYPE_TET)                                       // TODO: finish implementing BL with tets
//      getColumnTet(baseFace, p, elBaseEd, blob);
  }
  else if ((nbBaseFaceVert == 4) && (el->getType() == TYPE_HEX))                // Get BL column of hexas
    getColumnPrismHex(TYPE_HEX, face2el, p, elBaseFace, blob);
  else return false;                                                            // Not a BL
  if (blob.size() == 0) return false;                                           // Could not find column (may not be a BL)

  // Create top face of column with last face vertices
  topPrimVert.resize(nbBaseFaceVert);
  topPrimVert[0] = elBaseFace.getVertex(0);
  topPrimVert[1] = elBaseFace.getVertex(1);
  topPrimVert[2] = elBaseFace.getVertex(2);
  if (nbBaseFaceVert == 4) topPrimVert[3] = elBaseFace.getVertex(3);
  return true;
}



class DbgOutput {
public:
  void addMetaEl(MetaEl &mEl) {
    MElement *elt = mEl.getMElement();
    elType_.push_back(elt->getTypeForMSH());
    nbVertEl_.push_back(elt->getNumVertices());
    for (int iV = 0; iV < elt->getNumVertices(); iV++)
      point_.push_back(elt->getVertex(iV)->point());
  }
  void write(std::string fNameBase, int tag) {
    std::ostringstream oss;
    oss << fNameBase << "_" << tag << ".msh";
    std::string fName = oss.str();
    FILE *fp = fopen(fName.c_str(), "w");
    fprintf(fp, "$MeshFormat\n2.2 0 8\n$EndMeshFormat\n");
    fprintf(fp, "$Nodes\n");
    fprintf(fp, "%d\n", point_.size());
    for (int iV = 0; iV < point_.size(); iV++)
      fprintf(fp, "%i %g %g %g\n", iV+1, point_[iV].x(),
              point_[iV].y(), point_[iV].z());
    fprintf(fp, "$EndNodes\n");
    fprintf(fp, "$Elements\n");
    fprintf(fp, "%d\n", elType_.size());
    int iV = 0;
    for (int iEl = 0; iEl < elType_.size(); iEl++) {
      fprintf(fp, "%i %i 2 0 0 ", iEl+1, elType_[iEl]);
      for (int iVEl = 1; iVEl <= nbVertEl_[iEl]; iVEl++)
        fprintf(fp, " %i", iV+iVEl);
      fprintf(fp, "\n");
      iV += nbVertEl_[iEl];
    }
    fprintf(fp, "$EndElements\n");
    fclose(fp);
  }

private:
  std::vector<int> elType_;
  std::vector<int> nbVertEl_;
  std::vector<SPoint3> point_;
};



void curveMeshFromBnd(MEdgeVecMEltMap &ed2el, MFaceVecMEltMap &face2el,
                      GEntity *bndEnt, const FastCurvingParameters &p)
{
  // Build list of bnd. elements to consider
  std::list<MElement*> bndEl;
  if (bndEnt->dim() == 1) {
    GEdge *gEd = bndEnt->cast2Edge();
    for(unsigned int i = 0; i< gEd->lines.size(); i++)
      insertIfCurved(gEd->lines[i], bndEl);
  }
  else if (bndEnt->dim() == 2) {
    GFace *gFace = bndEnt->cast2Face();
    for(unsigned int i = 0; i< gFace->triangles.size(); i++)
      insertIfCurved(gFace->triangles[i], bndEl);
    for(unsigned int i = 0; i< gFace->quadrangles.size(); i++)
      insertIfCurved(gFace->quadrangles[i], bndEl);
  }
  else
    Msg::Error("Cannot treat model entity %i of dim %i", bndEnt->tag(),
                                                         bndEnt->dim());

  DbgOutput dbgOut;

  std::set<MVertex*> movedVert;
  for(std::list<MElement*>::iterator itBE = bndEl.begin();
      itBE != bndEl.end(); itBE++) {   // Loop over bnd. elements
    const int bndType = (*itBE)->getType();
    int metaElType;
    bool foundCol;
    std::vector<MVertex*> baseVert, topPrimVert;
    std::vector<MElement*> blob;
    if (bndType == TYPE_LIN) {                                                               // 1D boundary?
      MVertex *vb0 = (*itBE)->getVertex(0);
      MVertex *vb1 = (*itBE)->getVertex(1);
      metaElType = TYPE_QUA;
      MEdge baseEd(vb0, vb1);
      foundCol = getColumn2D(ed2el, p, baseEd, baseVert, topPrimVert, blob);
    }
    else {                                                                      // 2D boundary
      MVertex *vb0 = (*itBE)->getVertex(0);
      MVertex *vb1 = (*itBE)->getVertex(1);
      MVertex *vb2 = (*itBE)->getVertex(2);
      MVertex *vb3;
      if (bndType == TYPE_QUA) {
        vb3 = (*itBE)->getVertex(3);
        metaElType = TYPE_HEX;
      }
      else {
        vb3 = 0;
        metaElType = TYPE_PRI;
      }
      MFace baseFace(vb0, vb1, vb2, vb3);
      foundCol = getColumn3D(face2el, p, baseFace, baseVert, topPrimVert, blob);
    }
    if (!foundCol) continue;                                                    // Skip bnd. el. if top vertices not found
    int order = blob[0]->getPolynomialOrder();
    MetaEl metaEl(metaElType, order, baseVert, topPrimVert);
    dbgOut.addMetaEl(metaEl);
    for (int iEl = 0; iEl < blob.size(); iEl++) {
      MElement *&elt = blob[iEl];
      makeStraight(elt, movedVert);
      for (int iV = elt->getNumPrimaryVertices();
           iV < elt->getNumVertices(); iV++) {                                  // Loop over HO vert. of each el. in blob
        MVertex* vert = elt->getVertex(iV);
        if (movedVert.find(vert) == movedVert.end()) {                          // Try to move vert. not already moved
          double xyzS[3] = {vert->x(), vert->y(), vert->z()}, xyzC[3];
          if (metaEl.straightToCurved(xyzS,xyzC)) {
            vert->setXYZ(xyzC[0], xyzC[1], xyzC[2]);
            movedVert.insert(vert);
          }
        }
      }
    }
  }

  dbgOut.write("meta-elements", bndEnt->tag());
}



}



// Main function for fast curving
void HighOrderMeshFastCurving(GModel *gm, FastCurvingParameters &p)
{
  double t1 = Cpu();

  Msg::StatusBar(true, "Optimizing high order mesh...");
  std::vector<GEntity*> allEntities;
  gm->getEntities(allEntities);

  // Compute vert. -> elt. connectivity
  Msg::Info("Computing connectivity...");
  MEdgeVecMEltMap ed2el;
  MFaceVecMEltMap face2el;
  for (int iEnt = 0; iEnt < allEntities.size(); ++iEnt)
    if (p.dim == 2) calcEdge2Elements(allEntities[iEnt], ed2el);
    else calcFace2Elements(allEntities[iEnt], face2el);

  // Build multimap of each geometric entity to its boundaries
  std::multimap<GEntity*,GEntity*> entities;
  for (int iEnt = 0; iEnt < allEntities.size(); ++iEnt) {
    GEntity *dummy = 0;
    GEntity* &entity = allEntities[iEnt];
    if (entity->dim() == p.dim-1 &&
       (!p.onlyVisible || entity->getVisibility()) &&
       (entity->geomType() != GEntity::Plane))                                  // Consider non-plane boundary entities
      entities.insert(std::pair<GEntity*,GEntity*>(dummy, entity));
  }

  // Loop over geometric entities
  for (std::multimap<GEntity*,GEntity*>::iterator itBE = entities.begin();
       itBE != entities.end(); itBE++) {
    GEntity *domEnt = itBE->first, *bndEnt = itBE->second;
    Msg::Info("Curving elements for boundary entity %d...", bndEnt->tag());
    curveMeshFromBnd(ed2el, face2el, bndEnt, p);
  }

  double t2 = Cpu();

  Msg::StatusBar(true, "Done curving high order mesh (%g s)", t2-t1);
}
