
#ifndef mesh_h_
#define mesh_h_

#include <vector>
#include <string>
#include <algorithm>
#include <map>
#include <set>

#include "mpi.h"
#include "input.h"
#include "imc_state.h"
#include "element.h"
#include "constants.h"
#include "request.h"


using std::vector;
using std::endl;
using std::cout;
using std::string;
using std::sort;
using std::map;
using std::set;

using Constants::c;
using Constants::a;
using Constants::dir_type;
using Constants::bc_type;
using Constants::VACUUM; using Constants::REFLECT; using Constants::ELEMENT;
using Constants::cell_tag; using Constants::cell_id_tag;


namespace mpi = boost::mpi;


class Mesh {

  public:

  Mesh(Input* input)
  : ngx(input->get_n_x_elements()),
    ngy(input->get_n_y_elements()),
    ngz(input->get_n_z_elements())
  {
    max_map_size = input->get_map_size();
    double dx = input->get_dx();
    double dy = input->get_dy();
    double dz = input->get_dz();

    vector<bc_type> bc(6);
    bc[X_POS] = input->get_bc(X_POS); bc[X_NEG] = input->get_bc(X_NEG);
    bc[Y_POS] = input->get_bc(Y_POS); bc[Y_NEG] = input->get_bc(Y_NEG);
    bc[Z_POS] = input->get_bc(Z_POS); bc[Z_NEG] = input->get_bc(Z_NEG);

    //number of ranks
    n_rank =MPI::COMM_WORLD.Get_size();
    rank = MPI::COMM_WORLD.Get_rank();

    //initialize number of DMA requests as zero
    off_rank_reads=0;

    unsigned int g_count =0; //global count

    //this rank's elements
    n_global = ngx*ngy*ngz;
    unsigned int elem_id_begin = floor(rank*n_global/double(n_rank));
    unsigned int elem_id_end = floor((rank+1)*n_global/double(n_rank));

    unsigned int l_count =0;
    for (unsigned int k=0; k<ngz; k++) {
      for (unsigned int j=0; j<ngy; j++) {
        for (unsigned int i=0; i<ngx; i++) {
          if (g_count >= elem_id_begin && g_count < elem_id_end) {
            //global_ID.push_back(count*10 + MPI::COMM_WORLD.Get_rank()  );
            Element e;
            e.set_coor(i*dx, (i+1)*dx, j*dy, (j+1)*dy, k*dz, (k+1)*dz);
            e.set_ID(g_count);
            e.set_cV(input->get_CV());
            e.set_T_e(input->get_initial_Tm()); 
            e.set_T_r(input->get_initial_Tr());
            e.set_T_s(0.0);
            e.set_rho(input->get_rho());

            if (i<(ngx-1)) {e.set_neighbor( X_POS, g_count+1); e.set_bc(X_POS, ELEMENT);}
            else                {e.set_neighbor( X_POS, g_count); e.set_bc(X_POS, bc[X_POS]);} 

            if (i>0)            {e.set_neighbor( X_NEG, g_count-1); e.set_bc(X_NEG, ELEMENT);}
            else                {e.set_neighbor( X_NEG, g_count); e.set_bc(X_NEG, bc[X_NEG]);}

            if (j<(ngy-1))      {e.set_neighbor(Y_POS, g_count+ngx); e.set_bc(Y_POS, ELEMENT);}
            else                {e.set_neighbor(Y_POS, g_count); e.set_bc(Y_POS, bc[Y_POS]);}

            if (j>0)            {e.set_neighbor(Y_NEG, g_count-ngx); e.set_bc(Y_NEG, ELEMENT);}
            else                {e.set_neighbor(Y_NEG, g_count); e.set_bc(Y_NEG, bc[Y_NEG]);}

            if (k<(ngz-1)) {e.set_neighbor(Z_POS, g_count+ngx*ngy); e.set_bc(Z_POS, ELEMENT);}
            else                {e.set_neighbor(Z_POS, g_count); e.set_bc(Z_POS, bc[Z_POS]);}  

            if (k>0)            {e.set_neighbor(Z_NEG, g_count-ngx*ngy); e.set_bc(Z_NEG, ELEMENT);}
            else                {e.set_neighbor(Z_NEG, g_count); e.set_bc(Z_NEG, bc[Z_NEG]);}
            elem_list.push_back(e);
            l_count++;
          }
          g_count++;
        } //end i loop
      } // end j loop
    } // end k loop
    n_element = l_count;
    m_opA = input->get_opacity_A();
    m_opB = input->get_opacity_B();
    m_opC = input->get_opacity_C();
    m_opS = input->get_opacity_S();

    total_photon_E = 0.0;

    //define the MPI element type used for MPI windows
    //make the MPI datatype for my element class
    // Three type entries in the class
    const int entry_count = 3 ; 
    // 7 unsigned int, 6 int, 13 double
    const int array_of_block_length[4] = {8, 6, 14};
    // Displacements of each type in the element
    const MPI::Aint array_of_block_displace[3] = {0, 8*sizeof(unsigned int),  8*sizeof(unsigned int)+6*sizeof(int)};
    //Type of each memory block
    MPI::Datatype array_of_types[3] = {MPI_UNSIGNED, MPI_INT, MPI_DOUBLE}; 

    MPI_Element = MPI::Datatype::Create_struct(entry_count,
      array_of_block_length, array_of_block_displace, array_of_types);
    MPI_Element.Commit();

    //bool flags to say if data is needed
    need_data = vector<bool>(n_rank-1, false);
    send_data = vector<bool>(n_rank-1, false);

    //allocate mpi requests objects
    r_cell_reqs = new Request[ (n_rank-1)];
    r_cell_ids_reqs = new Request[ (n_rank-1)];
    s_cell_reqs = new Request[ (n_rank-1)];
    s_cell_ids_reqs = new Request[ (n_rank-1)];
   
    b_r_cell_reqs    = vector<bool> (n_rank-1, false);
    b_r_cell_ids_reqs = vector<bool> (n_rank-1, false);
    b_s_cell_reqs    = vector<bool> (n_rank-1, false);
    b_s_cell_ids_reqs = vector<bool> (n_rank-1, false);

    // size the send and receive buffers
    // they are size n_rank -1
    for (unsigned int ir=0; ir<n_rank-1; ir++) {
      vector<Element> empty_vec_elem;
      r_cells.push_back(empty_vec_elem);
      s_cells.push_back(empty_vec_elem);
      vector<unsigned int> empty_vec_ids;
      r_cell_ids.push_back(empty_vec_ids);
      s_cell_ids.push_back(empty_vec_ids);
      //ids needed from other ranks
      ids_needed.push_back(empty_vec_ids); 
    }
    

  }

  //free requests and delete MPI allocated element
  ~Mesh() { 
    delete[] r_cell_reqs;
    delete[] r_cell_ids_reqs;
    delete[] s_cell_reqs;
    delete[] s_cell_ids_reqs;
  }

/*****************************************************************************/
  //const functions
/*****************************************************************************/
  unsigned int get_number_of_objects(void) const {return n_element;}
  unsigned int get_global_ID(unsigned int index) const {return  elem_list[index].get_ID();}
  unsigned int get_rank(void) const {return  rank;}
  unsigned int get_offset(void) const {return on_rank_start;}
  unsigned int get_global_num_elements(void) const {return n_global;}
  void  get_xyz(unsigned int index, double *pos) const { elem_list[index].get_xyz(pos);} 
  double get_total_photon_E(void) const {return total_photon_E;}

  void print(void) {
    for (unsigned int i= 0; i<n_element; i++)
      elem_list[i].print();
  }


  map<unsigned int, unsigned int> get_map(void) const {
    map<unsigned int, unsigned int> local_map;
    unsigned int g_ID;
    for (unsigned int i=0; i<n_element; i++) {
      g_ID = elem_list[i].get_ID();
      local_map[g_ID] = i+on_rank_start;
    }
    return local_map;
  }

  bool on_processor(const unsigned int& index) const { 
    return  (index>=on_rank_start) && (index<=on_rank_end) ; 
  }

  Element get_pre_elem(const unsigned int& local_ID) const {return elem_list[local_ID];} 
  Element get_elem(const unsigned int& local_ID) const {return elements[local_ID];} 

  void print_map(void) {
    for ( map<unsigned int,Element>::iterator map_i = stored_elements.begin();
      map_i!=stored_elements.end(); map_i++) 
      (map_i->second).print();
  }

  unsigned int get_off_rank_id(const unsigned int& index) const {
    //find rank of index
    bool found = false;
    unsigned int min_i = 0;
    unsigned int max_i = off_rank_bounds.size()-1;
    unsigned int s_i; //search index
    while(!found) {
      s_i =(max_i + min_i)/2;
      if (s_i == max_i || s_i == min_i) found = true;
      else if (index >= off_rank_bounds[s_i]) min_i = s_i;
      else max_i = s_i;
    }
    return s_i;
  }

  unsigned int get_rank(const unsigned int& index) const {
    unsigned int r_rank;
    if (on_processor(index)) r_rank = rank;
    else  r_rank = get_off_rank_id(index);
    return r_rank;
  }

  bool mesh_available(const unsigned int& index) const {
    if (on_processor(index)) return true;
    else if (stored_elements.find(index) != stored_elements.end())
      return true;
    else 
      return false; 
  } 

  Element get_on_rank_element(const unsigned int& index) {
    //this can only be called with valid on rank indexes
    if (on_processor(index)) 
      return elements[index-on_rank_start];
    else 
      return stored_elements[index];
  }

/*****************************************************************************/
  //non-const functions
/*****************************************************************************/
  void set_global_bound(unsigned int _on_rank_start, unsigned int _on_rank_end) {
    on_rank_start = _on_rank_start;
    on_rank_end = _on_rank_end;
  }

  void set_off_rank_bounds(vector<unsigned int> _off_rank_bounds) {
    off_rank_bounds=_off_rank_bounds;
  }

  void calculate_photon_energy(IMC_State* imc_s) {
    total_photon_E = 0.0;
    double dt = imc_s->get_dt();
    double op_a, op_s, f, cV;
    double vol;
    double T, Tr, Ts;
    unsigned int step = imc_s->get_step();
    double tot_census_E = 0.0;
    double tot_emission_E = 0.0;
    double tot_source_E = 0.0;
    double pre_mat_E = 0.0;
    for (unsigned int i=0; i<n_element;++i) {
      Element& e = elements[i];
      vol = e.get_volume();
      cV = e.get_cV();
      T = e.get_T_e();
      Tr = e.get_T_r();
      Ts = e.get_T_s();
      op_a = m_opA + m_opB*pow(T, m_opC);
      op_s = m_opS;
      f =1.0/(1.0 + dt*op_a*c*(4.0*a*pow(T,3)/cV));

      e.set_op_a(op_a);
      e.set_op_s(op_s);
      e.set_f(f);

      m_emission_E[i] = dt*vol*f*op_a*a*c*pow(T,4);
      if (step > 1) m_census_E[i] = 0.0;  
      else m_census_E[i] =vol*a*pow(Tr,4); 
      m_source_E[i] = dt*op_a*a*c*pow(Ts,4);

      pre_mat_E+=T*cV*vol;
      tot_emission_E+=m_emission_E[i];
      tot_census_E  +=m_census_E[i];
      tot_source_E  +=m_source_E[i];
      total_photon_E += m_emission_E[i] + m_census_E[i] + m_source_E[i];
    }

    //set conservation
    imc_s->set_pre_mat_E(pre_mat_E);
    imc_s->set_emission_E(tot_emission_E);
    imc_s->set_source_E(tot_source_E);
    if(imc_s->get_step() == 1) imc_s->set_pre_census_E(tot_census_E);
  }


  void set_indices( map<unsigned int, unsigned int> off_map, 
                    vector< vector<bool> >& remap_flag) {
    unsigned int next_index;
    map<unsigned int, unsigned int>::iterator end = off_map.end();
    unsigned int new_index;
    //check to see if neighbors are on or off processor
    for (unsigned int i=0; i<n_element; i++) {
      Element& elem = elem_list[i];
      for (unsigned int d=0; d<6; d++) {
        next_index = elem.get_next_element(d);
        map<unsigned int, unsigned int>::iterator map_i = off_map.find(next_index);
        if (off_map.find(next_index) != end && remap_flag[i][d] ==false ) {
          //update index and bc type, this will always be an off processor so
          //if an index is updated it will always be at a processor bound
          remap_flag[i][d] = true;
          new_index = map_i->second;
          elem.set_neighbor( dir_type(d) , new_index );
          elem.set_bc(dir_type(d), PROCESSOR);
          boundary_elements.push_back(new_index);
        }
      }
    }
  }

  void set_local_indices(map<unsigned int, unsigned int> local_map) {
    unsigned int next_index;
    map<unsigned int, unsigned int>::iterator end = local_map.end();
    unsigned int new_index;
    bc_type current_bc;
    //check to see if neighbors are on or off processor
    for (unsigned int i=0; i<n_element; i++) {
      Element& elem = elem_list[i];
      elem.set_ID(i+on_rank_start);
      for (unsigned int d=0; d<6; d++) {
        current_bc = elem.get_bc(bc_type(d));
        next_index = elem.get_next_element(d);
        map<unsigned int, unsigned int>::iterator map_i = local_map.find(next_index);
        //if this index is not a processor boundary, update it
        if (local_map.find(next_index) != end && current_bc != PROCESSOR) {
          new_index = map_i->second;
          elem.set_neighbor( dir_type(d) , new_index );
        }
      } // end direction
    } // end element
  }

  void update_mesh(void) {
    vector<Element> new_mesh;
    for (unsigned int i =0; i< elem_list.size(); i++) {
      bool delete_flag = false;
      for (vector<unsigned int>::iterator rmv_itr= remove_elem_list.begin(); rmv_itr != remove_elem_list.end(); rmv_itr++) {
        if (*rmv_itr == i)  delete_flag = true;
      }
      if (delete_flag == false) new_mesh.push_back(elem_list[i]);
    }

    for (unsigned int i =0; i< new_elem_list.size(); i++) new_mesh.push_back(new_elem_list[i]);
    elem_list = new_mesh;
    n_element = elem_list.size();
    new_elem_list.clear();
    remove_elem_list.clear();
    sort(elem_list.begin(), elem_list.end());

    //use the final number of elements to size vectors
    m_census_E = vector<double>(n_element, 0.0);
    m_emission_E = vector<double>(n_element, 0.0);
    m_source_E = vector<double>(n_element, 0.0);
  }

  void make_MPI_window(void) {
    //make the MPI window with the sorted element list
    unsigned int num_bytes =n_element*MPI_Element.Get_size();
    elements = (Element*) MPI::Alloc_mem(num_bytes, MPI_INFO_NULL);
    memcpy(elements,&elem_list[0], num_bytes);
    
    elem_list.clear();
  }


  void update_temperature(vector<double>& abs_E, IMC_State* imc_s) {
    //abs E is a global vector
    double total_abs_E = 0.0;
    double total_post_mat_E = 0.0;
    double vol,cV,rho,T, T_new;
    for (unsigned int i=0; i<n_element;++i) {
      Element& e = elements[i];
      vol = e.get_volume();
      cV = e.get_cV();
      T = e.get_T_e();
      rho = e.get_rho();
      T_new = T + (abs_E[i+on_rank_start] - m_emission_E[i])/(cV*vol*rho);
      e.set_T_e(T_new);
      total_abs_E+=abs_E[i+on_rank_start];
      total_post_mat_E+= T_new*cV*vol;
    }
    //zero out absorption tallies for all elements (global) 
    for (unsigned int i=0; i<abs_E.size();++i) {
      abs_E[i] = 0.0;
    }
    imc_s->set_absorbed_E(total_abs_E);
    imc_s->set_post_mat_E(total_post_mat_E);
    imc_s->set_off_rank_read(off_rank_reads);
    off_rank_reads = 0;
  }


  void request_element(const unsigned int& index) {
    //get local index of global index
    unsigned int off_rank_id = get_off_rank_id(index);
    unsigned int off_rank_local = index - off_rank_bounds[off_rank_id];
    //get correct index into received photon vector
    unsigned int r_index = off_rank_id - (off_rank_id>rank);
    //add to the request list if not already requested (use global index)
    if (ids_requested.find(index) == ids_requested.end()) {
      ids_requested.insert(index);
      ids_needed[r_index].push_back(off_rank_local);
      //need data from off_rank_id that has not already been requested
      need_data[r_index] = true;
      //increment number of RMA requests
      off_rank_reads++;
    }
  }


  bool process_mesh_requests(mpi::communicator world) {
    bool new_data = false;
    for (unsigned int ir=0; ir<n_rank; ir++) {
      if (ir != rank) {
        //get correct index into requests and vectors 
        unsigned int r_index = ir - (ir>rank);
        
        ////////////////////////////////////////////////////////////////////////
        // if you need data from this rank, process request
        ////////////////////////////////////////////////////////////////////////
        if (need_data[r_index]) {
          //if you haven't requested cells, do that
          if (!b_s_cell_ids_reqs[r_index]) {
            //copy needed cells to the send buffer
            s_cell_ids[r_index].assign(ids_needed[r_index].begin(), ids_needed[r_index].end());
            //cout<<"Number of cells needed by "<<rank<<" from "<<ir<<" is " <<ids_needed[r_index].size()<<endl;
            //cout<<"Total IDS requested by rank "<<rank<<" is: "<<off_rank_reads<<endl;
            s_cell_ids_reqs[r_index].request( world.isend(ir, cell_id_tag, s_cell_ids[r_index]));
            b_s_cell_ids_reqs[r_index] = true;
            //clear requested cell ids (requested cells will not be requested again
            //because they are still stored in requested_ids set
            ids_needed[r_index].clear();
          }
          //otherwise, check to see if send completed, then reset buffer
          //post receive for this rank, if not done
          if (!b_r_cell_reqs[r_index]) {
            r_cell_reqs[r_index].request(world.irecv(ir, cell_tag, r_cells[r_index]));
            b_r_cell_reqs[r_index] = true;
          }
          else {
            //check for completion
            if (r_cell_reqs[r_index].test()) {
              //reset send and receive flags and need_data flag 
              b_s_cell_ids_reqs[r_index] = false;
              b_r_cell_reqs[r_index] = false;
              if (ids_needed[r_index].empty()) need_data[r_index] = false;
              new_data = true;

              //add received cells to working mesh
              for (unsigned int i=0; i<r_cells[r_index].size();i++) {
                unsigned int index = r_cells[r_index][i].get_ID();
                //add this element to the map, if possible, otherwise manage map
                if (stored_elements.size() < max_map_size) stored_elements[index] = r_cells[r_index][i];
                else {
                  //remove from map and from requests so it can be reqeusted again if needed
                  unsigned int removed_id = (stored_elements.begin())->first ;
                  ids_requested.erase(removed_id);
                  stored_elements.erase(stored_elements.begin());
                  stored_elements[index] = r_cells[r_index][i];
                }
              } // for i in r_cells[r_index]
              r_cells[r_index].clear();
              s_cell_ids[r_index].clear();
            } // if r_cells_reqs[r_index].test()
          }
        } // if need_data[r_index]


        ////////////////////////////////////////////////////////////////////////
        // receiving element ids needed by other ranks (post receives to all)
        ////////////////////////////////////////////////////////////////////////
        // check to see if receive call made
        if (!b_r_cell_ids_reqs[r_index]) {
          r_cell_ids_reqs[r_index].request(world.irecv(ir, cell_id_tag, r_cell_ids[r_index]));
          b_r_cell_ids_reqs[r_index] = true;
        }
        // add cell ids to requested for a rank
        else if (!send_data[r_index]) {
          if (r_cell_ids_reqs[r_index].test())  {
            //send data to this rank
            send_data[r_index] = true;
            //make cell send list for this rank
            for (unsigned int i=0; i<r_cell_ids[r_index].size();i++)
              s_cells[r_index].push_back(elements[r_cell_ids[r_index][i]]);
          } // if (r_cell_ids[r_index].test() )
        }
   
        //send elements needed by other ranks
        //check to see if this rank need your data
        if (send_data[r_index]) {
          if(!b_s_cell_reqs[r_index]) {
            //cout<<"Number of cells sent by "<<rank<<" to "<<ir<<" is " <<s_cells[r_index].size()<<endl;
            s_cell_reqs[r_index].request(world.isend(ir, cell_tag, s_cells[r_index]));
            b_s_cell_reqs[r_index] = true;
          }
          else {
            //check for completion of send message
            if (s_cell_reqs[r_index].test()) {
              //data is no longer needed by this rank, buffers can be reused
              // and receive and send messages can be posted again
              send_data[r_index] = false;
              b_r_cell_ids_reqs[r_index] = false;
              b_s_cell_reqs[r_index] = false;
              r_cell_ids[r_index].clear();
              s_cells[r_index].clear();
            }
          }
        }
      } //if (ir != rank)
    } //for ir in rank
    return new_data;
  }

  /*
  void clear_messages(void) {
    for (unsigned int ir=0; ir<n_rank; ir++) {
      if (ir != rank) {
        //get correct index into requests and vectors 
        unsigned int r_index = ir - (ir>rank);
        b_r_cell_ids_reqs[r_index] = false;
        b_s_cell_ids_reqs[r_index] = false;
        b_r_cell_reqs[r_index] = false;
        b_s_cell_reqs[r_index] = false;
        r_cell_ids_reqs[r_index].cancel();
        s_cell_ids_reqs[r_index].cancel();
        r_cell_reqs[r_index].cancel();
        s_cell_reqs[r_index].cancel();
      }
    }
  }
  */

  void purge_working_mesh(void) {
    stored_elements.clear(); ghost_elements.clear();
    ids_requested.clear();  
  }

  void add_mesh_elem(Element new_elem) {new_elem_list.push_back(new_elem);}
  void remove_elem(unsigned int index) {remove_elem_list.push_back(index);}

  vector<double>& get_census_E_ref(void) {return m_census_E;}
  vector<double>& get_emission_E_ref(void) {return m_emission_E;}
  vector<double>& get_source_E_ref(void) {return m_source_E;}

/*****************************************************************************/
  //member variables
/*****************************************************************************/
  private:

  unsigned int ngx;
  unsigned int ngy;
  unsigned int ngz;

  unsigned int max_map_size;

  unsigned int rank; //! MPI rank of this mesh
  unsigned int n_rank; //! Number of global ranks

  unsigned int off_rank_reads; //! Number of off rank reads

  unsigned int n_element; //! Number of local elements
  unsigned int n_global; //! Nuber of global elements
  
  unsigned int on_rank_start; //! Start of global index on rank
  unsigned int on_rank_end;   //! End of global index on rank

  vector<double> m_census_E; //! Census energy vector
  vector<double> m_emission_E; //! Emission energy vector
  vector<double> m_source_E; //! Source energy vector

  Element *elements; //! Element data allocated with MPI_Alloc
  vector<Element> elem_list;
  vector<Element> new_elem_list;
  vector<unsigned int> remove_elem_list;
  vector<unsigned int> off_rank_bounds; //! Ending value of global ID for each rank
  vector<unsigned int> boundary_elements; //! Index of adjacent ghost cells

  map<unsigned int, Element> stored_elements; //! Elements that have been accessed off rank
  map<unsigned int, Element> ghost_elements; //! Static list of off-rank elements next to boundary

  vector<vector<unsigned int> > ids_needed ;   //! Cell needed by this rank
  set<unsigned int> ids_requested; //! IDs that have been requested

  //send and receive buffers
  vector< vector<Element> > r_cells;          //! Receive buffer for cells
  vector<vector <unsigned int> > r_cell_ids;  //! Receive cell ids needed by other ranks
  vector<vector< Element> > s_cells;          //! Cells to send to each rank
  vector<vector<unsigned int> > s_cell_ids;   //! Send buffer for cell ids needed by this rank

  //Data bools needed from off rank and other ranks that need data here
  vector<bool> need_data;
  vector<bool> send_data;

  //MPI requests for non-blocking communication and the bools for if the request has been made
  //receive requests and bool flags
  Request* r_cell_reqs; //! Received cell requests
  vector<bool>  b_r_cell_reqs; //! Bool for received call requests
  Request* r_cell_ids_reqs; //! Received cell id requests
  vector<bool>  b_r_cell_ids_reqs; //! Bool for received call requests
  //send requests and bool flags
  Request* s_cell_reqs; //! Sent cell requests
  vector<bool>  b_s_cell_reqs; //! Bool for received call requests
  Request* s_cell_ids_reqs; //! Sent cell id requests
  vector<bool>  b_s_cell_ids_reqs; //! Bool for received call requests

  double m_opA;
  double m_opB;
  double m_opC;
  double m_opS;

  double total_photon_E;

  MPI::Datatype MPI_Element;
};

#endif //mesh_h_
