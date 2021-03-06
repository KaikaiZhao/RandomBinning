// This program performs multiclass classification based on the
// one-vs-all scheme. In each binary classification problem, the
// classifer is a kernel ridge regression. The kernel ridge
// regresssion is approximated by using the Random Binning feature
// method. This program is suitable for the case of a large number of
// classes.
//
// This program implements the following kernels (lambda is the
// regularization):
//
// IsotropicLaplace: k(x,y) = exp(-r/sigma) + lambda,
//     where r = sqrt[ sum_i (x_i-y_i)^2 ].
//
// This program uses the LibSVM data format. The attribute indices
// must start from 1. The class labels must be consecutive integers
// starting from 0.
//
// This program can be run with more than one thread. Most parts of
// the program have been threaded except for data I/O and random
// number generation. The program accepts the following compile-time
// macros: -DUSE_OPENBLAS -DUSE_OPENMP .
//
// Usage:
//
//   KRR_OneVsAll_RandBin.ex NumThreads FileTrain FileTest NumClasses 
//   d r Seed Num_lambda List_lambda Num_sigma List_sigma
//
//   NumThreads:  Number of threads
//   FileTrain:   File name (including path) of train data
//   FileTest:    File name (including path) of test data
//   NumClasses:  Number of classes
//   d:           Data dimension
//   r:           Rank (number of random features)
//   Seed:        Seed for RNG. If < 0, use current time
//   Kernel:      One of IsotropicGaussian, IsotropicLaplace, ProdLaplace
//   lambda:      
//   Num_lambda:  Number of lambda's for parameter tuning
//   List_lambda: List of lambda's (Regularization)
//   Num_sigma:   Number of sigma's for parameter tuning
//   List_sigma:  List of sigma's (Kernel function)

#include "randFeature.hpp"
#include "LibCMatrix.hpp"

//--------------------------------------------------------------------------
typedef enum { IsotropicGaussian, IsotropicLaplace, ProdLaplace } KERNEL;

#define TWO_PI 6.28318530717958647692

//--------------------------------------------------------------------------
//int ReadData(const char *filename, double **X, int **y, int d, int *n);
//int VerifyFileFormat(const char *filename, int d, int *n);
void UniformRandom01(double *a, int n);
void StandardNormal(double *a, int n);
void StudentT1(double *a, int n);
void MultivariateStudentT1(double *a, int n);

// For multiclass classification, need to convert a single vector
// ytest to a matrix Ytest.
void ConvertYtrain(const DVector &ytrain, DMatrix &Ytrain, int NumClasses);

// If NumClasses = 1 (regression), return relative error. If
// NumClasses = 2 (binary classification), return accuracy% (between 0
// and 100).
double Performance(const DVector &ytest_truth, const DVector &ytest_predict,
                   int NumClasses);

// If Numclasses > 2 (multiclass classification), must call this
// routine. The input Ytest_predict consists of NumClasses
// columns. Return accuracy% (between 0 and 100).
double Performance(const DVector &ytest_truth, const DMatrix &Ytest_predict,
                   int NumClasses);

//--------------------------------------------------------------------------
int main(int argc, char **argv) {

  // Temporary variables
  long int i, j, k, ii, idx = 1;

  // Arguments
  int NumThreads = atoi(argv[idx++]);
  char *FileTrain = argv[idx++];
  char *FileTest = argv[idx++];
  int NumClasses = atoi(argv[idx++]);
  int d = atoi(argv[idx++]);
  int r = atoi(argv[idx++]);
  int Num_lambda = atoi(argv[idx++]);
  double *List_lambda = (double *)malloc(Num_lambda*sizeof(double));
  for (ii = 0; ii < Num_lambda; ii++) {
    List_lambda[ii] = atof(argv[idx++]);
  }
  int Num_sigma = atoi(argv[idx++]);
  double *List_sigma = (double *)malloc(Num_sigma*sizeof(double));
  for (i = 0; i < Num_sigma; i++) {
    List_sigma[i] = atof(argv[idx++]);
  }
  int MAXIT = atoi(argv[idx++]);
  double TOL = atof(argv[idx++]);
  bool verbose = atoi(argv[idx++]);

  // Threading
#ifdef USE_OPENBLAS
  openblas_set_num_threads(NumThreads);
#elif USE_ESSL

#elif USE_OPENMP
  omp_set_num_threads(NumThreads);
#else
  NumThreads = 1; // To avoid compiler warining of unused variable
#endif

  PREPARE_CLOCK(1);
  START_CLOCK;

  // Read in X = Xtrain (n*d), y = ytrain (n*1),
  //     and X0 = Xtest (m*d), y0 = ytest (m*1)
  DPointArray Xtrain;        // read all data points from train
  DPointArray Xtest;        // read all data points from test
  DVector ytrain;           // Training labels
  DVector ytest;            // Testing labels (ground truth)

  if (ReadData(FileTrain, Xtrain, ytrain, d) == 0) {
    return -1;
  }
  if (ReadData(FileTest, Xtest, ytest, d) == 0) {
    return -1;
  }

  END_CLOCK;
  printf("RandBinning: time loading data = %g seconds, n train = %ld, m test = %ld, num threads = %d\n", 
        ELAPSED_TIME, Xtrain.GetN(), Xtest.GetN(), NumThreads); fflush(stdout);

  // For multiclass classification, need to convert a single vector
  // ytrain to a matrix Ytrain. The "predictions" are stored in the
  // corresponding matrix Ytest_predict. The vector ytest_predict is
  DMatrix Ytrain;
  if (NumClasses > 2){
    ConvertYtrain(ytrain, Ytrain, NumClasses);
  }

  int Seed = 0; // initialize seed as zero
  // Loop over List_lambda
  for (ii = 0; ii < Num_lambda; ii++) {
    double lambda = List_lambda[ii];
    // Loop over List_sigma
    for (k = 0; k < Num_sigma; k++) {
    double sigma = List_sigma[k];
    // Seed the RNG
    srandom(Seed);

    double TimeTrain = 0;
    double TimeTest = 0;
    START_CLOCK;
    // Generate feature matrix Xdata_randbin given Xdata
    vector< vector< pair<int,double> > > instances_old, instances_new;
    long Xtrain_N = Xtrain.GetN();
    for(i=0;i<Xtrain_N;i++){
      instances_old.push_back(vector<pair<int,double> >());
      for(j=0;j<d;j++){
        int index = j+1;
        double *myXtrain = Xtrain.GetPointer();
        double  myXtrain_feature = myXtrain[j*Xtrain_N+i];
        if (myXtrain_feature != 0)
          instances_old.back().push_back(pair<int,double>(index, myXtrain_feature));
      }
    }
    long Xtest_N = Xtest.GetN();
    for(i=0;i<Xtest_N;i++){
      instances_old.push_back(vector<pair<int,double> >());
      for(j=0;j<d;j++){
        int index = j+1;
        double *myXtest = Xtest.GetPointer();
        double  myXtest_feature = myXtest[j*Xtest_N+i];
        if (myXtest_feature != 0)
          instances_old.back().push_back(pair<int,double>(index, myXtest_feature));
      }
    }
    END_CLOCK;
    TimeTrain += ELAPSED_TIME;
    printf("RandBinning: Train. Time (in seconds) for converting data format: %g\n", ELAPSED_TIME);fflush(stdout);
     
     // add 0 feature for Enxu's code
    START_CLOCK;
    double gamma = 1/sigma;
    random_binning_feature(d+1, r, instances_old, instances_new, sigma);
    END_CLOCK;
    TimeTrain += ELAPSED_TIME;
    printf("RandBinning: Train. Time (in seconds) for generating random binning features: %g\n", ELAPSED_TIME);fflush(stdout);

    START_CLOCK;
    SPointArray Xdata_randbin;  // Generate random binning features
    long int nnz = r*(Xtrain_N + Xtest_N);
    long int dd = 0;
    for(i = 0; i < instances_new.size(); i++){
      if(dd < instances_new[i][r-1].first)
        dd = instances_new[i][r-1].first;
    }
    Xdata_randbin.Init(Xtrain_N+Xtest_N, dd, nnz);
    long int ind = 0;
    long int *mystart = Xdata_randbin.GetPointerStart();
    int *myidx = Xdata_randbin.GetPointerIdx();
    double *myX = Xdata_randbin.GetPointerX();
    for(i = 0; i < instances_new.size(); i++){
      if (i == 0)
        mystart[i] = 0;
      else
        mystart[i] = mystart[i-1] + r;
      for(j = 0; j < instances_new[i].size(); j++){
        myidx[ind] = instances_new[i][j].first-1;
        myX[ind] = instances_new[i][j].second;
        ind++;
      }
    }
    mystart[i] = nnz; // mystart has a length N+1
    // generate random binning features for Xtrain and Xtest
    SPointArray Xtrain;         // Training points
    SPointArray Xtest;          // Testing points
    long Row_start = 0;
    Xdata_randbin.GetSubset(Row_start, Xtrain_N,Xtrain);
    Xdata_randbin.GetSubset(Xtrain_N,Xtest_N,Xtest);
    Xdata_randbin.ReleaseAllMemory();
    END_CLOCK;
    TimeTrain += ELAPSED_TIME;
    printf("RandBinning: Train. Time (in seconds) for converting data format back: %g\n", ELAPSED_TIME);fflush(stdout);

    /* Set up Training and Testing */
    int m; // number of classes
    long N = Xtest.GetN(); // number of testing points
    long M = Xtest.GetD(); // dimension of randome binning features
    DMatrix Ytest_predict; // predictions for multiclasses
    DMatrix W; // weights for multiclasses
    DVector ytest_predict; // prediction for binary classification or regression
    DVector w; // weights for binary classification or regression
    w.Init(M); 
    if (NumClasses > 2){
        m = Ytrain.GetN(); 
        Ytest_predict.Init(N,m);
        W.Init(M,m);
    }
    else{
        m = 1; 
        ytest_predict.Init(N); 
    }

    // Create an identity matrix as preconditioner, could be removed later
    SPointArray EYE;
    EYE.Init(M,M,M);
    mystart = EYE.GetPointerStart();
    myidx = EYE.GetPointerIdx();
    myX = EYE.GetPointerX();
    for(i=0;i<M;i++){
      mystart[i] = i;
      myidx[i] = i;
      myX[i] = 1;
    }
    mystart[i] = M+1; // mystart has a length M+1

    // Start Training L2R-SquareLoss model:
    // solve (Z'Z + lambdaI)w = Z'y, note that we never explicitly form
    // Z'Z since Z is a large sparse matrix N*dd
    START_CLOCK;
    DVector yy; // holding yy = Z'y
    for (i = 0; i < m; i++) {
       if (NumClasses > 2){ 
          Ytrain.GetColumn(i, ytrain);
       }
       Xtrain.MatVec(ytrain, yy, TRANSPOSE);
       double NormRHS = yy.Norm2();
       PCG pcg_solver;
       pcg_solver.Solve<SPointArray, SPointArray>(Xtrain, yy, w, EYE, MAXIT, TOL, 1, lambda);
       if (verbose) {
         int Iter = 0;
         const double *ResHistory = pcg_solver.GetResHistory(Iter);
         printf("RandBinning: Train. PCG: iteration = %d, Relative residual = %g\n",
            Iter, ResHistory[Iter-1]/NormRHS);fflush(stdout);
       }
       pcg_solver.GetSolution(w);
       if (NumClasses > 2){
          W.SetColumn(i, w);
       }
    }
    END_CLOCK;
    TimeTrain += ELAPSED_TIME;
    printf("RandBinning: Train. Time (in seconds) for solving linear system solution: %g\n", ELAPSED_TIME);fflush(stdout);

    // Start Testing L2R-SquareLoss model:
    // y = Xtest*W = z(x)'*w
    START_CLOCK;
    double accuracy = 0.0;
    if (NumClasses > 2){
       Xtest.MatMat(W,Ytest_predict,NORMAL,NORMAL);
       accuracy = Performance(ytest, Ytest_predict, NumClasses);
    }
    else {
       Xtest.MatVec(w,ytest_predict,NORMAL);
       accuracy = Performance(ytest, ytest_predict, NumClasses);
    }
    END_CLOCK;
    TimeTest += ELAPSED_TIME;
    printf("RandBinning: OneVsAll. r = %d, D = %ld, param = %g %g, perf = %g, time = %g %g\n", 
            r, dd, sigma, lambda, accuracy, TimeTrain, TimeTest); fflush(stdout);
  }// End loop over List_sigma
  }// End loop over List_lambda

  // Clean up
  free(List_sigma);
  free(List_lambda);

  return 0;
}


//--------------------------------------------------------------------------
void UniformRandom01(double *a, int n) {
  int i;
  for (i = 0; i < n; i++) {
    a[i] = (double)random()/RAND_MAX;
  }
}


//--------------------------------------------------------------------------
// Box-Muller: U, V from uniform[0,1]
// X = sqrt(-2.0*log(U))*cos(2.0*M_PI*V)
// Y = sqrt(-2.0*log(U))*sin(2.0*M_PI*V)
void StandardNormal(double *a, int n) {
  int i;
  for (i = 0; i < n/2; i++) {
    double U = (double)random()/RAND_MAX;
    double V = (double)random()/RAND_MAX;
    double common1 = sqrt(-2.0*log(U));
    double common2 = TWO_PI*V;
    a[2*i] = common1 * cos(common2);
    a[2*i+1] = common1 * sin(common2);
  }
  if (n%2 == 1) {
    double U = (double)random()/RAND_MAX;
    double V = (double)random()/RAND_MAX;
    double common1 = sqrt(-2.0*log(U));
    double common2 = TWO_PI*V;
    a[n-1] = common1 * cos(common2);
  }
}


//--------------------------------------------------------------------------
// Let X and Y be independent standard normal. Then X/fabs(Y) follows
// student t with 1 degree of freedom.
void StudentT1(double *a, int n) {
  int i;
  for (i = 0; i < n; i++) {
    double V = (double)random()/RAND_MAX;
    a[i] = tan(2.0*M_PI*V); // Why is cot not in math.h??? :-(
    if (V > 0.5) {
      a[i] = -a[i];
    }
  }
}


//--------------------------------------------------------------------------
// Let X and Y be independent standard normal. Then X/fabs(Y) follows
// student t with 1 degree of freedom.
void MultivariateStudentT1(double *a, int n) {
  StandardNormal(a, n);
  double b = 0.0;
  StandardNormal(&b, 1);
  b = fabs(b);
  int i;
  for (i = 0; i < n; i++) {
    a[i] /= b;
  }
}


//--------------------------------------------------------------------------
void ConvertYtrain(const DVector &ytrain, DMatrix &Ytrain, int NumClasses) {
  Ytrain.Init(ytrain.GetN(), NumClasses);
  // Lingfei: the y label is supposed to start from 0.
  for (int i = 0; i < NumClasses; i++) {
    DVector y = ytrain;
    double *my = y.GetPointer();
    for (long j = 0; j < y.GetN(); j++) {
      my[j] = ((int)my[j])==i ? 1.0 : -1.0;
    }
    Ytrain.SetColumn(i, y);
  }
}


//--------------------------------------------------------------------------
double Performance(const DVector &ytest_truth, const DVector &ytest_predict,
                   int NumClasses) {

  long n = ytest_truth.GetN();
  if (n != ytest_predict.GetN()) {
    printf("Performance. Error: Vector lengths mismatch. Return NAN");
    return NAN;
  }
  if (NumClasses != 1 && NumClasses != 2) {
    printf("Performance. Error: Neither regression nor binary classification. Return NAN");
    return NAN;
  }
  double perf = 0.0;

  if (NumClasses == 1) { // Relative error
    DVector diff;
    ytest_truth.Subtract(ytest_predict, diff);
    perf = diff.Norm2()/ytest_truth.Norm2();
  }
  else if (NumClasses == 2) { // Accuracy
    double *y1 = ytest_truth.GetPointer();
    double *y2 = ytest_predict.GetPointer();
    for (long i = 0; i < n; i++) {
      perf += y1[i]*y2[i]>0 ? 1.0:0.0;
    }
    perf = perf/n * 100.0;
  }

  return perf;
}


//--------------------------------------------------------------------------
double Performance(const DVector &ytest_truth, const DMatrix &Ytest_predict,
                   int NumClasses) {

  long n = ytest_truth.GetN();
  if (n != Ytest_predict.GetM() || Ytest_predict.GetN() != NumClasses ) {
    printf("Performance. Error: Size mismatch. Return NAN");
    return NAN;
  }
  if (NumClasses <= 2) {
    printf("Performance. Error: Not multiclass classification. Return NAN");
    return NAN;
  }
  double perf = 0.0;

  // Compute ytest_predict
  DVector ytest_predict(n);
  double *y = ytest_predict.GetPointer();
  for (long i = 0; i < n; i++) {
    DVector row(NumClasses);
    Ytest_predict.GetRow(i, row);
    long idx = -1;
    row.Max(idx);
    y[i] = (double)idx;
  }

  // Accuracy
  double *y1 = ytest_truth.GetPointer();
  double *y2 = ytest_predict.GetPointer();
  for (long i = 0; i < n; i++) {
    perf += ((int)y1[i])==((int)y2[i]) ? 1.0 : 0.0;
  }
  perf = perf/n * 100.0;

  return perf;
}

