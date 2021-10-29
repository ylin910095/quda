#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <quda.h>
#include <quda_internal.h>
#include <gauge_field.h>

#include <comm_quda.h>
#include <host_utils.h>
#include <command_line_params.h>
#include <misc.h>
#include <timer.h>

#include <gauge_tools.h>
#include <tune_quda.h>

#include <pgauge_monte.h>
#include <random_quda.h>
#include <unitarization_links.h>

#include <qio_field.h>

#include <gtest/gtest.h>

using namespace quda;

//***********************************************************//
// This boolean controls whether or not the full Google test //
// is done. If the user passes a value of 1 or 2 to --test   //
// then a single instance of OVR or FFT gauge fixing is done //
// and the value of this bool is set to false. Otherwise the //
// Google tests are performed.                               //
//***********************************************************//
bool execute = true;

// Gauge IO related
bool gauge_load;
bool gauge_store;
void *host_gauge[4];

void display_test_info()
{
  printfQuda("running the following test:\n");

  switch (test_type) {
  case 0: printfQuda("\n Google testing\n"); break;
  case 1: printfQuda("\nOVR gauge fix\n"); break;
  case 2: printfQuda("\nFFT gauge fix\n"); break;
  default: errorQuda("Undefined test type %d given", test_type);
  }

  printfQuda("prec    sloppy_prec    link_recon  sloppy_link_recon S_dimension T_dimension Ls_dimension\n");
  printfQuda("%s   %s             %s            %s            %d/%d/%d          %d         %d\n", get_prec_str(prec),
             get_prec_str(prec_sloppy), get_recon_str(link_recon), get_recon_str(link_recon_sloppy), xdim, ydim, zdim,
             tdim, Lsdim);

  printfQuda("Grid partition info:     X  Y  Z  T\n");
  printfQuda("                         %d  %d  %d  %d\n", dimPartitioned(0), dimPartitioned(1), dimPartitioned(2),
             dimPartitioned(3));
}

// Define the command line options and option group for this test
int gf_gauge_dir = 4;
int gf_maxiter = 10000;
int gf_verbosity_interval = 100;
double gf_ovr_relaxation_boost = 1.5;
double gf_fft_alpha = 0.8;
int gf_reunit_interval = 10;
double gf_tolerance = 1e-6;
bool gf_theta_condition = false;

void add_gaugefix_option_group(std::shared_ptr<QUDAApp> quda_app)
{
  // Option group for gauge fixing related options
  auto opgroup = quda_app->add_option_group("gaugefix", "Options controlling gauge fixing tests");
  opgroup->add_option("--gf-dir", gf_gauge_dir,
                      "The orthogonal direction of teh gauge fixing, 3=Coulomb, 4=Landau. (default 4)");
  opgroup->add_option("--gf-maxiter", gf_maxiter,
                      "The maximun number of gauge fixing iterations to be applied (default 10000) ");
  opgroup->add_option("--gf-verbosity-interval", gf_verbosity_interval,
                      "Print the gauge fixing progress every N steps (default 100)");
  opgroup->add_option("--gf-ovr-relaxation-boost", gf_ovr_relaxation_boost,
                      "The overrelaxation boost parameter for the overrelaxation method (default 1.5)");
  opgroup->add_option("--gf-fft-alpha", gf_fft_alpha, "The Alpha parameter in the FFT method (default 0.8)");
  opgroup->add_option("--gf-reunit-interval", gf_reunit_interval,
                      "Reunitarise the gauge field every N steps (default 10)");
  opgroup->add_option("--gf-tol", gf_tolerance, "The tolerance of the gauge fixing quality (default 1e-6)");
  opgroup->add_option(
    "--gf-theta-condition", gf_theta_condition,
    "Use the theta value to determine the gauge fixing if true. If false, use the delta value (default false)");
}

class GaugeAlgTest : public ::testing::Test {

protected:
  QudaGaugeParam param;

  device_timer_t device_timer_1, device_timer_2;
  double2 detu;
  double3 plaq;
  cudaGaugeField *U;
  int nsteps;
  int nhbsteps;
  int novrsteps;
  bool coldstart;
  double beta_value;
  RNG * randstates;
  
  void SetReunitarizationConsts(){
    const double unitarize_eps = 1e-14;
    const double max_error = 1e-10;
    const int reunit_allow_svd = 1;
    const int reunit_svd_only  = 0;
    const double svd_rel_error = 1e-6;
    const double svd_abs_error = 1e-6;
    setUnitarizeLinksConstants(unitarize_eps, max_error,
                               reunit_allow_svd, reunit_svd_only,
                               svd_rel_error, svd_abs_error);

  }

  bool checkDimsPartitioned()
  {
    if (comm_dim_partitioned(0) || comm_dim_partitioned(1) || comm_dim_partitioned(2) || comm_dim_partitioned(3))
      return true;
    return false;
  }

  bool comparePlaquette(double3 a, double3 b){
    double a0,a1,a2;
    a0 = std::abs(a.x - b.x);
    a1 = std::abs(a.y - b.y);
    a2 = std::abs(a.z - b.z);
    double prec_val = 1.0e-5;
    if (prec == QUDA_DOUBLE_PRECISION) prec_val = gf_tolerance * 1e2;
    return ((a0 < prec_val) && (a1 < prec_val) && (a2 < prec_val));
  }

  bool CheckDeterminant(double2 detu){
    double prec_val = 5e-8;
    if (prec == QUDA_DOUBLE_PRECISION) prec_val = gf_tolerance * 1e2;
    return (std::abs(1.0 - detu.x) < prec_val && std::abs(detu.y) < prec_val);
  }

  virtual void SetUp()
  {
    if (execute) {
      setVerbosity(QUDA_VERBOSE);
      param = newQudaGaugeParam();

      // Setup gauge container.
      setWilsonGaugeParam(param);
      param.t_boundary = QUDA_PERIODIC_T;

      // Reunitarization setup
      int *num_failures_h = (int *)mapped_malloc(sizeof(int));
      int *num_failures_d = (int *)get_mapped_device_pointer(num_failures_h);
      SetReunitarizationConsts();

      device_timer_1.start();

      // If no field is loaded, create a physical quenched field on the device
      if (!gauge_load) {
        GaugeFieldParam gParam(param);
        gParam.ghostExchange = QUDA_GHOST_EXCHANGE_EXTENDED;
        gParam.create = QUDA_NULL_FIELD_CREATE;
        gParam.reconstruct = link_recon;
        gParam.setPrecision(prec, true);
        for (int d = 0; d < 4; d++) {
          if (comm_dim_partitioned(d)) gParam.r[d] = 2;
          gParam.x[d] += 2 * gParam.r[d];
        }

        U = new cudaGaugeField(gParam);

        RNG randstates(*U, 1234);

        nsteps = heatbath_num_steps;
        nhbsteps = heatbath_num_heatbath_per_step;
        novrsteps = heatbath_num_overrelax_per_step;
        coldstart = heatbath_coldstart;
        beta_value = heatbath_beta_value;
        device_timer_2.start();

        if (coldstart)
          InitGaugeField(*U);
        else
          InitGaugeField(*U, randstates);

        for (int step = 1; step <= nsteps; ++step) {
          printfQuda("Step %d\n", step);
          Monte(*U, randstates, beta_value, nhbsteps, novrsteps);

          // Reunitarization
          *num_failures_h = 0;
          unitarizeLinks(*U, num_failures_d);
          qudaDeviceSynchronize();
          if (*num_failures_h > 0) errorQuda("Error in the unitarization (%d errors)", *num_failures_h);
          plaq = plaquette(*U);
          printfQuda("Plaq: %.16e, %.16e, %.16e\n", plaq.x, plaq.y, plaq.z);
        }

        device_timer_2.stop();
        printfQuda("Time Monte -> %.6f s\n", device_timer_2.last());
      } else {

        // If a field is loaded, create a device field and copy
        printfQuda("Copying gauge field from host\n");
        param.location = QUDA_CPU_FIELD_LOCATION;
        GaugeFieldParam gauge_field_param(param, host_gauge);
        gauge_field_param.ghostExchange = QUDA_GHOST_EXCHANGE_NO;
        GaugeField *host = GaugeField::Create(gauge_field_param);

        // switch the parameters for creating the mirror precise cuda gauge field
        gauge_field_param.create = QUDA_NULL_FIELD_CREATE;
        gauge_field_param.reconstruct = param.reconstruct;
        gauge_field_param.setPrecision(param.cuda_prec, true);

        if (comm_partitioned()) {
          int R[4] = {0, 0, 0, 0};
          for (int d = 0; d < 4; d++)
            if (comm_dim_partitioned(d)) R[d] = 2;
          static TimeProfile GaugeFix("GaugeFix");
          cudaGaugeField *tmp = new cudaGaugeField(gauge_field_param);
          tmp->copy(*host);
          U = createExtendedGauge(*tmp, R, GaugeFix);
          delete tmp;
        } else {
          U = new cudaGaugeField(gauge_field_param);
          U->copy(*host);
        }
	
        delete host;

        // Reunitarization
        *num_failures_h = 0;
        unitarizeLinks(*U, num_failures_d);
        qudaDeviceSynchronize();
        if (*num_failures_h > 0) errorQuda("Error in the unitarization (%d errors)", *num_failures_h);

        plaq = plaquette(*U);
        printfQuda("Plaq: %.16e, %.16e, %.16e\n", plaq.x, plaq.y, plaq.z);
      }

      // If a specific test type is requested, perfrom it now and then
      // turn off all Google tests in the tear down.
      switch (test_type) {
      case 0:
        // Do the Google testing
        break;
      case 1: run_ovr(); break;
      case 2: run_fft(); break;
      default: errorQuda("Invalid test type %d ", test_type);
      }
      
      host_free(num_failures_h);
    }
  }

  virtual void TearDown()
  {
    if (execute) {
      detu = getLinkDeterminant(*U);
      double2 tru = getLinkTrace(*U);
      printfQuda("Det: %.16e:%.16e\n", detu.x, detu.y);
      printfQuda("Tr: %.16e:%.16e\n", tru.x / 3.0, tru.y / 3.0);

      delete U;
      // Release all temporary memory used for data exchange between GPUs in multi-GPU mode
      PGaugeExchangeFree();

      device_timer_1.stop();
      printfQuda("Time -> %.6f s\n", device_timer_1.last());
    }
    // If we performed a specific instance, switch off the
    // Google testing.
    if (test_type != 0) execute = false;
  }
  
  virtual void run_ovr()
  {
    if (execute) {
      printfQuda("%s gauge fixing with overrelaxation method\n",  gf_gauge_dir == 4 ? "Landau" : "Coulomb");
      gaugeFixingOVR(*U, gf_gauge_dir, gf_maxiter, gf_verbosity_interval, gf_ovr_relaxation_boost, gf_tolerance,
                     gf_reunit_interval, gf_theta_condition);
      auto plaq_gf = plaquette(*U);
      printfQuda("Plaq:    %.16e, %.16e, %.16e\n", plaq.x, plaq.y, plaq.z);
      printfQuda("Plaq GF: %.16e, %.16e, %.16e\n", plaq_gf.x, plaq_gf.y, plaq_gf.z);
      ASSERT_TRUE(comparePlaquette(plaq, plaq_gf));
      saveTuneCache();
      // Save if output string is specified
      if (gauge_store) save_gauge();
    }
  }
  virtual void run_fft()
  {
    if (execute) {
      if (!checkDimsPartitioned()) {
        printfQuda("%s gauge fixing with steepest descent method with FFT\n", gf_gauge_dir == 4 ? "Landau" : "Coulomb");
	// We hardcode the value of autotune to 1 in the kernel call (lib/gauge_fix_fft.cu)
	// This ensures that the user can not override alpha autotuning. This is done because
	// it is very easy for the FFT gauge fixing to fail with a poorly chosen value of
	// alpha, but autotuning alpha ensures optimal behaviour.
	// Users who wish to change this behaviour may read the comment in
	// lib/gauge_fix_fft.cu to regain control.
	gaugeFixingFFT(*U, gf_gauge_dir, gf_maxiter, gf_verbosity_interval, gf_fft_alpha, 1, gf_tolerance,
                       gf_theta_condition);

        auto plaq_gf = plaquette(*U);
        printfQuda("Plaq:    %.16e, %.16e, %.16e\n", plaq.x, plaq.y, plaq.z);
        printfQuda("Plaq GF: %.16e, %.16e, %.16e\n", plaq_gf.x, plaq_gf.y, plaq_gf.z);
        ASSERT_TRUE(comparePlaquette(plaq, plaq_gf));
        saveTuneCache();
        // Save if output string is specified
        if (gauge_store) save_gauge();
      } else {
        errorQuda("Cannot perform FFT gauge fixing with MPI partitions.");
      }
    }
  }

  virtual void save_gauge()
  {
    printfQuda("Saving the gauge field to file %s\n", gauge_outfile);

    QudaGaugeParam gauge_param = newQudaGaugeParam();
    setWilsonGaugeParam(gauge_param);

    void *cpu_gauge[4];
    for (int dir = 0; dir < 4; dir++) { cpu_gauge[dir] = safe_malloc(V * gauge_site_size * gauge_param.cpu_prec); }

    GaugeFieldParam gParam(param);
    gParam.ghostExchange = QUDA_GHOST_EXCHANGE_NO;
    gParam.create = QUDA_NULL_FIELD_CREATE;
    gParam.link_type = param.type;
    gParam.reconstruct = param.reconstruct;
    gParam.setPrecision(gParam.Precision(), true);

    cudaGaugeField *gauge;
    gauge = new cudaGaugeField(gParam);

    // copy into regular field
    copyExtendedGauge(*gauge, *U, QUDA_CUDA_FIELD_LOCATION);
    saveGaugeFieldQuda((void *)cpu_gauge, (void *)gauge, &gauge_param);

    // Write to disk
    write_gauge_field(gauge_outfile, cpu_gauge, gauge_param.cpu_prec, gauge_param.X, 0, (char **)0);

    for (int dir = 0; dir < 4; dir++) host_free(cpu_gauge[dir]);
    delete gauge;
  }
};

TEST_F(GaugeAlgTest, Generation)
{
  if (execute && !gauge_load) {
    detu = getLinkDeterminant(*U);
    ASSERT_TRUE(CheckDeterminant(detu));
  }
}

TEST_F(GaugeAlgTest, Landau_Overrelaxation)
{
  if (execute) {
    printfQuda("Landau gauge fixing with overrelaxation\n");
    gaugeFixingOVR(*U, 4, gf_maxiter, gf_verbosity_interval, gf_ovr_relaxation_boost, gf_tolerance, gf_reunit_interval,
                   gf_theta_condition);
    auto plaq_gf = plaquette(*U);
    printfQuda("Plaq:    %.16e, %.16e, %.16e\n", plaq.x, plaq.y, plaq.z);
    printfQuda("Plaq GF: %.16e, %.16e, %.16e\n", plaq_gf.x, plaq_gf.y, plaq_gf.z);
    ASSERT_TRUE(comparePlaquette(plaq, plaq_gf));
    saveTuneCache();
  }
}

TEST_F(GaugeAlgTest, Coulomb_Overrelaxation)
{
  if (execute) {
    printfQuda("Coulomb gauge fixing with overrelaxation\n");
    gaugeFixingOVR(*U, 3, gf_maxiter, gf_verbosity_interval, gf_ovr_relaxation_boost, gf_tolerance, gf_reunit_interval,
                   gf_theta_condition);
    auto plaq_gf = plaquette(*U);
    printfQuda("Plaq:    %.16e, %.16e, %.16e\n", plaq.x, plaq.y, plaq.z);
    printfQuda("Plaq GF: %.16e, %.16e, %.16e\n", plaq_gf.x, plaq_gf.y, plaq_gf.z);
    ASSERT_TRUE(comparePlaquette(plaq, plaq_gf));
    saveTuneCache();
  }
}

TEST_F(GaugeAlgTest, Landau_FFT)
{
  if (execute) {
    if (!comm_partitioned()) {
      printfQuda("Landau gauge fixing with steepest descent method with FFT\n");
      // We hardcode the value of autotune to 1 in the kernel call (lib/gauge_fix_fft.cu)
      // This ensures that the user can not override alpha autotuning. This is done because
      // it is very easy for the FFT gauge fixing to fail with a poorly chosen value of
      // alpha, but autotuning alpha ensures optimal behaviour.
      // Users who wish to change this behaviour may read the comment in
      // lib/gauge_fix_fft.cu to regain control.
      gaugeFixingFFT(*U, 4, gf_maxiter, gf_verbosity_interval, gf_fft_alpha, 1, gf_tolerance,
                     gf_theta_condition);
      auto plaq_gf = plaquette(*U);
      printfQuda("Plaq:    %.16e, %.16e, %.16e\n", plaq.x, plaq.y, plaq.z);
      printfQuda("Plaq GF: %.16e, %.16e, %.16e\n", plaq_gf.x, plaq_gf.y, plaq_gf.z);
      ASSERT_TRUE(comparePlaquette(plaq, plaq_gf));
      saveTuneCache();
    }
  }
}

TEST_F(GaugeAlgTest, Coulomb_FFT)
{
  if (execute) {
    if (!comm_partitioned()) {
      printfQuda("Coulomb gauge fixing with steepest descent method with FFT\n");
      // We hardcode the value of autotune to 1 in the kernel call (lib/gauge_fix_fft.cu)
      // This ensures that the user can not override alpha autotuning. This is done because
      // it is very easy for the FFT gauge fixing to fail with a poorly chosen value of
      // alpha, but autotuning alpha ensures optimal behaviour.
      // Users who wish to change this behaviour may read the comment in
      // lib/gauge_fix_fft.cu to regain control.
      gaugeFixingFFT(*U, 3, gf_maxiter, gf_verbosity_interval, gf_fft_alpha, 1, gf_tolerance,
                     gf_theta_condition);
auto plaq_gf = plaquette(*U);
      printfQuda("Plaq:    %.16e, %.16e, %.16e\n", plaq.x, plaq.y, plaq.z);
      printfQuda("Plaq GF: %.16e, %.16e, %.16e\n", plaq_gf.x, plaq_gf.y, plaq_gf.z);
      ASSERT_TRUE(comparePlaquette(plaq, plaq_gf));
      saveTuneCache();
    }
  }
}

int main(int argc, char **argv)
{
  // initalize google test, includes command line options
  ::testing::InitGoogleTest(&argc, argv);

  // command line options  
  auto app = make_app();
  add_gaugefix_option_group(app);
  add_heatbath_option_group(app);

  test_type = 0;
  CLI::TransformPairs<int> test_type_map {{"Google", 0}, {"OVR", 1}, {"FFT", 2}};
  app->add_option("--test", test_type, "Test method")->transform(CLI::CheckedTransformer(test_type_map));

  try {
    app->parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app->exit(e);
  }

  // initialize QMP/MPI, QUDA comms grid and RNG (host_utils.cpp)
  initComms(argc, argv, gridsize_from_cmdline);

  QudaGaugeParam gauge_param = newQudaGaugeParam();
  if (prec_sloppy == QUDA_INVALID_PRECISION) prec_sloppy = prec;
  if (link_recon_sloppy == QUDA_RECONSTRUCT_INVALID) link_recon_sloppy = link_recon;

  setWilsonGaugeParam(gauge_param);
  setDims(gauge_param.X);

  display_test_info();

  gauge_load = strcmp(latfile, "");
  gauge_store = strcmp(gauge_outfile, "");

  // If we are passing a gauge field to the test, we must allocate host memory.
  // If no gauge is passed, we generate a quenched field on the device.
  if (gauge_load) {
    printfQuda("Loading gauge field from host\n");
    for (int dir = 0; dir < 4; dir++) {
      host_gauge[dir] = safe_malloc(V * gauge_site_size * host_gauge_data_type_size);
    }
    constructHostGaugeField(host_gauge, gauge_param, argc, argv);
  }

  // call srand() with a rank-dependent seed
  initRand();

  // initialize the QUDA library
  initQuda(device_ordinal);

  // initalize google test, includes command line options
  ::testing::InitGoogleTest(&argc, argv);

  // Ensure gtest prints only from rank 0
  ::testing::TestEventListeners &listeners = ::testing::UnitTest::GetInstance()->listeners();
  if (comm_rank() != 0) { delete listeners.Release(listeners.default_result_printer()); }

  // return code for google test
  int test_rc = RUN_ALL_TESTS();
  if (gauge_load) {
    // release memory
    for (int dir = 0; dir < 4; dir++) host_free(host_gauge[dir]);
  }

  endQuda();

  finalizeComms();
  
  return test_rc;  
}