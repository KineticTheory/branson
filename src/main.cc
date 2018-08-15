//----------------------------------*-C++-*----------------------------------//
/*!
 * \file   main.cc
 * \author Alex Long
 * \date   July 24 2014
 * \brief  Reads input file, sets up mesh and runs transport
 * \note   Copyright (C) 2017 Los Alamos National Security, LLC.
 *         All rights reserved
 */
//---------------------------------------------------------------------------//

#include <iostream>
#include <mpi.h>
#include <string>
#include <vector>

#include "constants.h"
#include "decompose_mesh.h"
#include "imc_parameters.h"
#include "imc_state.h"
#include "info.h"
#include "input.h"
#include "mesh.h"
#include "mesh_pass_driver.h"
#include "mpi_types.h"
#include "particle_pass_driver.h"
#include "replicated_driver.h"
#include "rma_mesh_pass_driver.h"
#include "timer.h"

using std::vector;
using std::endl;
using std::cout;
using std::string;
using Constants::PARTICLE_PASS;
using Constants::CELL_PASS;
using Constants::CELL_PASS_RMA;
using Constants::REPLICATED;
using Constants::CUBE;
using Constants::PARMETIS;

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);

  // check to see if number of arguments is correct
  if (argc != 2) {
    cout << "Usage: BRANSON <path_to_input_file>" << endl;
    exit(EXIT_FAILURE);
  }

  // get MPI parmeters and set them in mpi_info
  const Info mpi_info;
  if (mpi_info.get_rank() == 0)
    cout << "Branson compiled on: " << mpi_info.get_machine_name() << endl;

  // make MPI types object
  MPI_Types *mpi_types = new MPI_Types();

  // get input object from filename
  std::string filename(argv[1]);
  Input *input;
  input = new Input(filename);
  if (mpi_info.get_rank() == 0)
    input->print_problem_info();

  // IMC paramters setup
  IMC_Parameters *imc_p;
  imc_p = new IMC_Parameters(input);

  // IMC state setup
  IMC_State *imc_state;
  imc_state = new IMC_State(input, mpi_info.get_rank());

  // timing
  Timer *timers = new Timer();

  // make mesh from input object
  timers->start_timer("Total setup");
  Mesh *mesh = new Mesh(input, mpi_types, mpi_info);

  // if mode is replicated ignore decomposition options, otherwise use parmetis
  // or a simple cube
  if (input->get_dd_mode() == REPLICATED)
    replicate_mesh(mesh, mpi_types, mpi_info, imc_p->get_grip_size());
  else if (input->get_decomposition_mode() == PARMETIS)
    decompose_mesh(mesh, mpi_types, mpi_info, imc_p->get_grip_size(), PARMETIS);
  else if (input->get_decomposition_mode() == CUBE)
    decompose_mesh(mesh, mpi_types, mpi_info, imc_p->get_grip_size(), CUBE);
  else {
    std::cout << "Method/decomposition not recognized, exiting...";
    exit(EXIT_FAILURE);
  }

  timers->stop_timer("Total setup");

  MPI_Barrier(MPI_COMM_WORLD);
  // print_MPI_out(mesh, rank, n_rank);

  //--------------------------------------------------------------------------//
  // TRT PHYSICS CALCULATION
  //--------------------------------------------------------------------------//

  timers->start_timer("Total transport");

  if (input->get_dd_mode() == PARTICLE_PASS)
    imc_particle_pass_driver(mesh, imc_state, imc_p, mpi_types, mpi_info);
  else if (input->get_dd_mode() == CELL_PASS)
    imc_mesh_pass_driver(mesh, imc_state, imc_p, mpi_types, mpi_info);
  else if (input->get_dd_mode() == CELL_PASS_RMA)
    imc_rma_mesh_pass_driver(mesh, imc_state, imc_p, mpi_types, mpi_info);
  else if (input->get_dd_mode() == REPLICATED)
    imc_replicated_driver(mesh, imc_state, imc_p, mpi_types, mpi_info);
  else
    cout << "Driver for DD transport method currently not supported" << endl;

  timers->stop_timer("Total transport");

  if (mpi_info.get_rank() == 0) {
    cout << "****************************************";
    cout << "****************************************" << endl;
    imc_state->print_simulation_footer(input->get_dd_mode());
    timers->print_timers();
  }
  MPI_Barrier(MPI_COMM_WORLD);

  delete mesh;
  delete timers;
  delete imc_state;
  delete imc_p;
  delete input;
  delete mpi_types; // destructor frees mpi types (calls MPI_type_free)

  MPI_Finalize();
}
//---------------------------------------------------------------------------//
// end of main.cc
//---------------------------------------------------------------------------//
