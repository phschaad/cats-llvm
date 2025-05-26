#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cstring>

// Basic GEMM kernel: C = A * B
void gemm(const float *A, const float *B, float *C, int N) {
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < N; j++) {
      float sum = 0.0;
      for (int k = 0; k < N; k++) {
        sum += A[i * N + k] * B[k * N + j];
      }
      C[i * N + j] = sum;
    }
  }
}

// Initialize matrix with random values
__attribute__((annotate("cats_noinstrument")))
void initializeRandom(float *matrix, int N) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<float> dist(0.0f, 1.0f);
  for (int i = 0; i < N * N; i++) {
    matrix[i] = dist(gen);
  }
}

// Print a small portion of the matrix (for verification)
__attribute__((annotate("cats_noinstrument")))
void printMatrixPreview(const float *matrix, int N, const char* name) {
  int preview_size = std::min(5, N);
  std::cout << "Matrix " << name << " preview (" << preview_size << "x" << preview_size << "):" << std::endl;
  for (int i = 0; i < preview_size; i++) {
    for (int j = 0; j < preview_size; j++) {
      std::cout << matrix[i * N + j] << " ";
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}

int main(int argc, char* argv[]) {
  // Default matrix size
  int N = 1024;

  // Parse command line arguments
  if (argc > 1) {
    N = std::atoi(argv[1]);
    if (N <= 0) {
      std::cerr << "Matrix size must be positive. Using default size: 1024" << std::endl;
      N = 1024;
    }
  }

  std::cout << "Performing " << N << "x" << N << " matrix multiplication..." << std::endl;

  // Allocate and initialize matrices
  std::cout << "Allocating matrices..." << std::endl;
  float *A = new float[N * N];
  float *B = new float[N * N];
  float *C = new float[N * N];

  std::cout << "Initializing matrices..." << std::endl;
  initializeRandom(A, N);
  initializeRandom(B, N);
  initializeRandom(C, N);

  std::cout << "Multiplying..." << std::endl;
  // Measure performance
  auto start = std::chrono::high_resolution_clock::now();

  // Perform matrix multiplication
  gemm(A, B, C, N);

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = end - start;

  // Print stats
  std::cout << "GEMM completed in " << elapsed.count() << " seconds" << std::endl;
  double gflops = (2.0 * N * N * N) / (elapsed.count() * 1e9);
  std::cout << "Performance: " << gflops << " GFLOPS" << std::endl;

  // Print small previews of the matrices (for verification)
  if (N <= 1024) {  // Only print for reasonably sized matrices
    printMatrixPreview(A, N, "A");
    printMatrixPreview(B, N, "B");
    printMatrixPreview(C, N, "C");
  }

  delete[] A;
  delete[] B;
  delete[] C;

  return 0;
}
