#ifndef VINA_VINAMPI_H
#define VINA_VINAMPI_H

typedef struct run_param_s {
    int random_seed;
    int seek_offset;
    int ligand_nbr;
} run_param;


struct rank_item {
    int idx;
    MPI_Request request;
    int* r;
};
#endif // ifdef VINA_VINAMPI_H
