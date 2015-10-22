from gmshpy import *
import sys

name = "Submarine"

g = GModel()
g.load(name + ".geo")
#g.mesh(2)
#g.save(name + "_KO.msh")
g.load(name + ".msh")



OH = MeshQualOptParameters()
OH.onlyVisible = False
OH.dim = 3
OH.fixBndNodes = False    # Fix boundary nodes or not
OH.strategy = 1           # 0 = Connected blobs, 1 = Adaptive one-by-one (recommended in 3D)

OH.excludeHex = False
OH.excludePrism = False
OH.nbLayers = 2          # Nb. of layers around invalid element to construct blob
OH.distanceFactor = 2    # Distance factor to construct blob
OH.maxPatchAdapt = 5       # Number of blob adaption iteration
OH.maxLayersAdaptFact = 3 # Factor to multiply nb. of layers when adapting
OH.distanceAdaptFact = 3 # Factor to multiply distance factor when adapting
#OH.weight = 0
OH.onlyValidity = False
#OH.minTargetIdealJac = 0.1
OH.minTargetInvCondNum = 0.2

#OH.weightFixed = 1.e-3
#OH.weightFree = 1.e-6

OH.nCurses = 0
OH.logFileName = "log"

OH.maxOptIter = 20             # Nb of optimixation iterations
OH.maxBarrierUpdates = 20        # Nb. of optimization passes

#print("minTargetIdealJac = %g" % OH.minTargetIdealJac)
print("minTargetInvCondNum = %g" % OH.minTargetInvCondNum)
print("nbLayers = %g, distanceFactor = %g" % (OH.nbLayers, OH.distanceFactor))
print("itMax = %g, optPassMax = %g" % (OH.maxOptIter, OH.maxBarrierUpdates))
#print("weightFree = %g, weightFixed = %g" % (OH.weightFree, OH.weightFixed))
print("strategy = %g" % OH.strategy)
print("maxAdaptBlob = %g" % OH.maxPatchAdapt)
print("maxLayersAdaptFact = %g, distanceAdaptFact = %g" % (OH.maxLayersAdaptFact, OH.distanceAdaptFact))

MeshQualityOptimizer(g, OH)

#print("RESULT: minNCJ = %g, maxNCJ = %g, CPU = %g" % (OH.minNCJ, OH.maxNCJ, OH.CPU))
#print("RESULT: minInvIdealJac = %g, maxInvIdealJac = %g, CPU = %g" %
#      (OH.minIdealJac, OH.maxIdealJac, OH.CPU))
print("RESULT: minInvCondNum = %g, maxInvCondNum = %g, CPU = %g" %
      (OH.minInvCondNum, OH.maxInvCondNum, OH.CPU))

g.save(name + "_opt.msh")
#g.writeMSH(name + "_OK.msh",3.0,False,False,False,1.0,0,0)

