/*

   Copyright (c) 2006-2010, The Scripps Research Institute

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   Author: Dr. Oleg Trott <ot14@columbia.edu>,
           The Olson Lab,
           The Scripps Research Institute

   This file is modified from QuickVina 2

*/

#include <iostream>
#include <fstream>
#include <string>
#include <exception>
#include <vector> // ligand paths
#include <cmath> // for ceila
#include <boost/program_options.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/convenience.hpp> // filesystem::basename
#include <boost/thread/thread.hpp> // hardware_concurrency // FIXME rm ?

#include <boost/date_time/posix_time/posix_time.hpp> // for time in microseconds
#include "parse_pdbqt.h"
#include "parallel_mc.h"
#include "file.h"
#include "cache.h"
#include "non_cache.h"
#include "naive_non_cache.h"
#include "parse_error.h"
#include "everything.h"
#include "weighted_terms.h"
#include "current_weights.h"
#include "quasi_newton.h"
#include "tee.h"
#include "coords.h" // add_to_output_container
//#include <ctime>

#include <queue>          // std::queue
#include <cstdio>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef SVINA_ENABLE_MPI

#include <mpi.h>
#include "vinampi.h"

#endif

using boost::filesystem::path;
using namespace boost::posix_time;

path make_path(const std::string& str) {
    return path(str);
}

void doing(int verbosity, const std::string& str, tee& log) {
    if(verbosity > 1) {
        log << str << std::string(" ... ");
        log.flush();
    }
}

void done(int verbosity, tee& log) {
    if(verbosity > 1) {
        log << "done.";
        log.endl();
    }
}
std::string default_output(const std::string& input_name) {
    std::string tmp = input_name;
    if(tmp.size() >= 6 && tmp.substr(tmp.size()-6, 6) == ".pdbqt")
        tmp.resize(tmp.size() - 6); // FIXME?
    return tmp + "_out.pdbqt";
}

void write_all_output(model& m, const output_container& out, sz how_many,
                      const std::string& output_name,
                      const std::vector<std::string>& remarks) {
    if(out.size() < how_many)
        how_many = out.size();
    VINA_CHECK(how_many <= remarks.size());
    ofile f(make_path(output_name));
    VINA_FOR(i, how_many) {
        m.set(out[i].c);
        m.write_model(f, i+1, remarks[i]); // so that model numbers start with 1
    }
}

void do_randomization(model& m,
                      const std::string& out_name,
                      const vec& corner1, const vec& corner2, int seed, int verbosity, tee& log) {
    conf init_conf = m.get_initial_conf();
    rng generator(static_cast<rng::result_type>(seed));
    if(verbosity > 1) {
        log << "Using random seed: " << seed;
        log.endl();
    }
    const sz attempts = 10000;
    conf best_conf = init_conf;
    fl best_clash_penalty = 0;
    VINA_FOR(i, attempts) {
        conf c = init_conf;
        c.randomize(corner1, corner2, generator);
        m.set(c);
        fl penalty = m.clash_penalty();
        if(i == 0 || penalty < best_clash_penalty) {
            best_conf = c;
            best_clash_penalty = penalty;
        }
    }
    m.set(best_conf);
    if(verbosity > 1) {
        log << "Clash penalty: " << best_clash_penalty; // FIXME rm?
        log.endl();
    }
    m.write_structure(make_path(out_name));
}

void refine_structure(model& m, const precalculate& prec, non_cache& nc, output_type& out, const vec& cap, sz max_steps = 1000) {
    change g(m.get_size());//initialized to be all zeros, and fit to the size of the model
    quasi_newton quasi_newton_par;
    quasi_newton_par.max_steps = max_steps;
    const fl slope_orig = nc.slope;
    VINA_FOR(p, 5) {
        nc.slope = 100 * std::pow(10.0, 2.0*p);
        quasi_newton_par(m, prec, nc, out, g, cap);
        m.set(out.c); // just to be sure
        if(nc.within(m))
            break;
    }
    out.coords = m.get_heavy_atom_movable_coords();
    if(!nc.within(m))
        out.e = max_fl;
    nc.slope = slope_orig;
}

std::string vina_remark(fl e, fl lb, fl ub) {
    std::ostringstream remark;
    remark.setf(std::ios::fixed, std::ios::floatfield);
    remark.setf(std::ios::showpoint);
    remark << "REMARK VINA RESULT: "
           << std::setw(9) << std::setprecision(1) << e
           << "  " << std::setw(9) << std::setprecision(3) << lb
           << "  " << std::setw(9) << std::setprecision(3) << ub
           << '\n';
    return remark.str();
}

output_container remove_redundant(const output_container& in, fl min_rmsd) {
    output_container tmp;
    VINA_FOR_IN(i, in)
    add_to_output_container(tmp, in[i], min_rmsd, in.size());
    return tmp;
}

void do_search(model& m, const boost::optional<model>& ref, const scoring_function& sf, const precalculate& prec, const igrid& ig, const precalculate& prec_widened, const igrid& ig_widened, non_cache& nc, // nc.slope is changed
               const std::string& out_name,
               const vec& corner1, const vec& corner2,
               const parallel_mc& par, fl energy_range, sz num_modes,
               int seed, int verbosity, bool score_only, bool local_only, tee& log, const terms& t, const flv& weights) {
    conf_size s = m.get_size();
    conf c = m.get_initial_conf();
    fl e = max_fl;
    const vec authentic_v(1000, 1000, 1000);
    if(score_only) {
        fl intramolecular_energy = m.eval_intramolecular(prec, authentic_v, c);
        naive_non_cache nnc(&prec); // for out of grid issues
        e = m.eval_adjusted(sf, prec, nnc, authentic_v, c, intramolecular_energy);
        log << "Affinity: " << std::fixed << std::setprecision(5) << e << " (kcal/mol)";
        log.endl();
        flv term_values = t.evale_robust(m);
        VINA_CHECK(term_values.size() == 5);
        log << "Intermolecular contributions to the terms, before weighting:\n";
        log << std::setprecision(5);
        log << "    gauss 1     : " << term_values[0] << '\n';
        log << "    gauss 2     : " << term_values[1] << '\n';
        log << "    repulsion   : " << term_values[2] << '\n';
        log << "    hydrophobic : " << term_values[3] << '\n';
        log << "    Hydrogen    : " << term_values[4] << '\n';
        VINA_CHECK(weights.size() == term_values.size() + 1);
        fl e2 = 0;
        VINA_FOR_IN(i, term_values)
        e2 += term_values[i] * weights[i];
        e2 = sf.conf_independent(m, e2);
        if(e < 100 && std::abs(e2 - e) > 0.05) {
            log << "WARNING: the individual terms are inconsisent with the\n";
            log << "WARNING: affinity. Consider reporting this as a bug:\n";
            log << "WARNING: http://vina.scripps.edu/manual.html#bugs\n";
        }
    }
    else if(local_only) {
        output_type out(c, e);
        doing(verbosity, "Performing local search", log);
        refine_structure(m, prec, nc, out, authentic_v, par.mc.ssd_par.evals);
        done(verbosity, log);
        fl intramolecular_energy = m.eval_intramolecular(prec, authentic_v, out.c);
        e = m.eval_adjusted(sf, prec, nc, authentic_v, out.c, intramolecular_energy);

        log << "Affinity: " << std::fixed << std::setprecision(5) << e << " (kcal/mol)";
        log.endl();
        if(!nc.within(m))
            log << "WARNING: not all movable atoms are within the search space\n";

        doing(verbosity, "Writing output", log);
        output_container out_cont;
        out_cont.push_back(new output_type(out));
        std::vector<std::string> remarks(1, vina_remark(e, 0, 0));
        write_all_output(m, out_cont, 1, out_name, remarks); // how_many == 1
        done(verbosity, log);
    }
    else {
        rng generator(static_cast<rng::result_type>(seed));
        log << "Using random seed: " << seed;
        log.endl();
        output_container out_cont;

//		time_t start,end;
        doing(verbosity, "Performing search", log);
        ptime time_start(microsec_clock::local_time());
//		time(&start);

        par(m, out_cont, prec, ig, prec_widened, ig_widened, corner1, corner2, generator);
        done(verbosity, log);

        doing(verbosity, "Refining results", log);
        VINA_FOR_IN(i, out_cont)
        refine_structure(m, prec, nc, out_cont[i], authentic_v, par.mc.ssd_par.evals);

        ptime time_end(microsec_clock::local_time());
        time_duration duration(time_end - time_start);
//		time(&end);
//		printf("\nsearching finished in %.3lf seconds\n",difftime(end,start));
        printf("\nsearching finished in %.3lf seconds\n",(duration.total_milliseconds()/1000.0));

        if(!out_cont.empty()) {
            out_cont.sort();
            const fl best_mode_intramolecular_energy = m.eval_intramolecular(prec, authentic_v, out_cont[0].c);
            VINA_FOR_IN(i, out_cont)
            if(not_max(out_cont[i].e))
                out_cont[i].e = m.eval_adjusted(sf, prec, nc, authentic_v, out_cont[i].c, best_mode_intramolecular_energy);
            // the order must not change because of non-decreasing g (see paper), but we'll re-sort in case g is non strictly increasing
            out_cont.sort();
        }

        const fl out_min_rmsd = 1;
        out_cont = remove_redundant(out_cont, out_min_rmsd);

        done(verbosity, log);

        log.setf(std::ios::fixed, std::ios::floatfield);
        log.setf(std::ios::showpoint);
        log << '\n';
        log << "mode |   affinity | dist from best mode\n";
        log << "     | (kcal/mol) | rmsd l.b.| rmsd u.b.\n";
        log << "-----+------------+----------+----------\n";

        model best_mode_model = m;
        if(!out_cont.empty())
            best_mode_model.set(out_cont.front().c);

        sz how_many = 0;
        std::vector<std::string> remarks;
        VINA_FOR_IN(i, out_cont) {
            if(how_many >= num_modes || !not_max(out_cont[i].e) || out_cont[i].e > out_cont[0].e + energy_range) break; // check energy_range sanity FIXME
            ++how_many;
            log << std::setw(4) << i+1
                << "    " << std::setw(9) << std::setprecision(1) << out_cont[i].e; // intermolecular_energies[i];
            m.set(out_cont[i].c);
            const model& r = ref ? ref.get() : best_mode_model;
            const fl lb = m.rmsd_lower_bound(r);
            const fl ub = m.rmsd_upper_bound(r);
            log << "  " << std::setw(9) << std::setprecision(3) << lb
                << "  " << std::setw(9) << std::setprecision(3) << ub; // FIXME need user-readable error messages in case of failures

            remarks.push_back(vina_remark(out_cont[i].e, lb, ub));
            log.endl();
        }
        doing(verbosity, "Writing output", log);
        write_all_output(m, out_cont, how_many, out_name, remarks);
        done(verbosity, log);

        if(how_many < 1) {
            log << "WARNING: Could not find any conformations completely within the search space.\n"
                << "WARNING: Check that it is large enough for all movable atoms, including those in the flexible side chains.";
            log.endl();
        }
    }
}

void main_procedure(model& m, const boost::optional<model>& ref, // m is non-const (FIXME?)
                    const std::string& out_name,
                    bool score_only, bool local_only, bool randomize_only, bool no_cache,
                    const grid_dims& gd, int exhaustiveness,
                    const flv& weights,
                    int cpu, int seed, int verbosity, sz num_modes, fl energy_range, tee& log) {

    doing(verbosity, "Setting up the scoring function", log);

    everything t;
    VINA_CHECK(weights.size() == 6);

    weighted_terms wt(&t, weights);
    precalculate prec(wt);
    const fl left  = 0.25;
    const fl right = 0.25;
    precalculate prec_widened(prec);
    prec_widened.widen(left, right);

    done(verbosity, log);

    vec corner1(gd[0].begin, gd[1].begin, gd[2].begin);
    vec corner2(gd[0].end,   gd[1].end,   gd[2].end);

    parallel_mc par;
    sz heuristic = m.num_movable_atoms() + 10 * m.get_size().num_degrees_of_freedom();
    par.mc.num_steps = unsigned(70 * 3 * (50 + heuristic) / 2); // 2 * 70 -> 8 * 20 // FIXME
    par.mc.ssd_par.evals = unsigned((25 + m.num_movable_atoms()) / 3);
    par.mc.min_rmsd = 1.0;
    par.mc.num_saved_mins = 20;
    par.mc.hunt_cap = vec(10, 10, 10);
    par.num_tasks = exhaustiveness;
    par.num_threads = cpu;
    par.display_progress = (verbosity > 1);

    const fl slope = 1e6; // FIXME: too large? used to be 100
    if(randomize_only) {
        do_randomization(m, out_name,
                         corner1, corner2, seed, verbosity, log);
    }
    else {
        non_cache nc        (m, gd, &prec,         slope); // if gd has 0 n's, this will not constrain anything
        non_cache nc_widened(m, gd, &prec_widened, slope); // if gd has 0 n's, this will not constrain anything
        if(no_cache) {
            do_search(m, ref, wt, prec, nc, prec_widened, nc_widened, nc,
                      out_name,
                      corner1, corner2,
                      par, energy_range, num_modes,
                      seed, verbosity, score_only, local_only, log, t, weights);
        }
        else {
            bool cache_needed = !(score_only || randomize_only || local_only);
            if(cache_needed) doing(verbosity, "Analyzing the binding site", log);
            cache c("scoring_function_version001", gd, slope, atom_type::XS);
            if(cache_needed) c.populate(m, prec, m.get_movable_atom_types(prec.atom_typing_used()));
            if(cache_needed) done(verbosity, log);
            do_search(m, ref, wt, prec, c, prec, c, nc,
                      out_name,
                      corner1, corner2,
                      par, energy_range, num_modes,
                      seed, verbosity, score_only, local_only, log, t, weights);
        }
    }
}

struct usage_error : public std::runtime_error {
    usage_error(const std::string& message) : std::runtime_error(message) {}
};

struct options_occurrence {
    bool some;
    bool all;
    options_occurrence() : some(false), all(true) {} // convenience
    options_occurrence& operator+=(const options_occurrence& x) {
        some = some || x.some;
        all  = all  && x.all;
        return *this;
    }
};

options_occurrence get_occurrence(boost::program_options::variables_map& vm, boost::program_options::options_description& d) {
    options_occurrence tmp;
    VINA_FOR_IN(i, d.options())
    if(vm.count((*d.options()[i]).long_name()))
        tmp.some = true;
    else
        tmp.all = false;
    return tmp;
}

void check_occurrence(boost::program_options::variables_map& vm, boost::program_options::options_description& d) {
    VINA_FOR_IN(i, d.options()) {
        const std::string& str = (*d.options()[i]).long_name();
        if(!vm.count(str))
            std::cerr << "Required parameter --" << str << " is missing!\n";
    }
}

model parse_bundle(const std::string& rigid_name, const boost::optional<std::string>& flex_name_opt, const std::vector<std::string>& ligand_names) {
    model tmp = (flex_name_opt) ? parse_receptor_pdbqt(make_path(rigid_name), make_path(flex_name_opt.get()))
                : parse_receptor_pdbqt(make_path(rigid_name));
    VINA_FOR_IN(i, ligand_names)
    tmp.append(parse_ligand_pdbqt(make_path(ligand_names[i])));
    return tmp;
}

model parse_bundle_partial_screening(const std::string& rigid_name/*const boost::optional<std::string>& flex_name_opt, const std::vector<std::string>& ligand_names*/) {
    model tmp = parse_receptor_pdbqt(make_path(rigid_name));
    /*
    model tmp = (flex_name_opt) ? parse_receptor_pdbqt(make_path(rigid_name), make_path(flex_name_opt.get()))
    	                        : parse_receptor_pdbqt(make_path(rigid_name));
    VINA_FOR_IN(i, ligand_names)
        tmp.append(parse_ligand_pdbqt(make_path(ligand_names[i])));
    */
    return tmp;
}

model parse_bundle(const std::vector<std::string>& ligand_names) {
    VINA_CHECK(!ligand_names.empty()); // FIXME check elsewhere
    model tmp = parse_ligand_pdbqt(make_path(ligand_names[0]));
    VINA_RANGE(i, 1, ligand_names.size())
    tmp.append(parse_ligand_pdbqt(make_path(ligand_names[i])));
    return tmp;
}

model parse_bundle(const boost::optional<std::string>& rigid_name_opt, const boost::optional<std::string>& flex_name_opt, const std::vector<std::string>& ligand_names) {
    if(rigid_name_opt)
        return parse_bundle(rigid_name_opt.get(), flex_name_opt, ligand_names);
    else
        return parse_bundle(ligand_names);
}

int main(int argc, char* argv[]) {
    using namespace boost::program_options;
    const std::string version_string = "AutoDock Vina 1.1.2 (May 11, 2011)";
    const std::string error_message = "\n\n\
Please contact the author, Dr. Oleg Trott <ot14@columbia.edu>, so\n\
that this problem can be resolved. The reproducibility of the\n\
error may be vital, so please remember to include the following in\n\
your problem report:\n\
* the EXACT error message,\n\
* your version of the program,\n\
* the type of computer system you are running it on,\n\
* all command line options,\n\
* configuration file (if used),\n\
* ligand file as PDBQT,\n\
* receptor file as PDBQT,\n\
* flexible side chains file as PDBQT (if used),\n\
* output file as PDBQT (if any),\n\
* input (if possible),\n\
* random seed the program used (this is printed when the program starts).\n\
\n\
Thank you!\n";


    const std::string cite_message = "\
############################################################################\n\
# If you used Quick Vina 2 in your work, please cite:                      #\n\
#                                                                          #\n\
# Amr Alhossary, Stephanus Daniel Handoko, Yuguang Mu, and Chee-Keong Kwoh,#\n\
# Fast, Accurate, and Reliable Molecular Docking with QuickVina 2,         #\n\
# Bioinformatics (2015), doi: 10.1093/bioinformatics/btv082                #\n\
#                                                                          #\n\
# You are also encouraged to cite Quick Vina 1:                            #\n\
# Stephanus Daniel Handoko, Xuchang Ouyang, Chinh Tran To Su, Chee Keong   #\n\
# Kwoh, Yew Soon Ong,                                                      #\n\
# QuickVina: Accelerating AutoDock Vina Using Gradient-Based Heuristics for#\n\
# Global Optimization,                                                     #\n\
# IEEE/ACM Transactions on Computational Biology and Bioinformatics,vol.9, #\n\
# no. 5, pp. 1266-1272, Sept.-Oct. 2012, doi: 10.1109/TCBB.2012.82         #\n\
#                                                                          #\n\
# and original AutoDock Vina paper:                                        #\n\
# O. Trott, A. J. Olson,                                                   #\n\
# AutoDock Vina: improving the speed and accuracy of docking with a        #\n\
# new scoring function, efficient optimization and multithreading,         #\n\
# Journal of Computational Chemistry 31 (2010) 455-461                     #\n\
# doi: 10.1002/jcc.21334                                                   #\n\
############################################################################\n\n*** This QVina has the screening additions (SVina) ***\n";

    try {
        std::string rigid_name, ligand_name, flex_name, config_name, out_name, log_name, job_file, batch_out;
        fl center_x, center_y, center_z, size_x, size_y, size_z;
        int cpu = 0, seed, exhaustiveness, verbosity = 2, num_modes = 9;
        int forknbr = 1;
        fl energy_range = 2.0;

        // -0.035579, -0.005156, 0.840245, -0.035069, -0.587439, 0.05846
        fl weight_gauss1      = -0.035579;
        fl weight_gauss2      = -0.005156;
        fl weight_repulsion   =  0.840245;
        fl weight_hydrophobic = -0.035069;
        fl weight_hydrogen    = -0.587439;
        fl weight_rot         =  0.05846;
        bool score_only = false, local_only = false, randomize_only = false, help = false, help_advanced = false, version = false; // FIXME

        bool batchMode = false;
        bool use_fork_parallelism = false;
        bool use_mpi_parallelism = false;

        positional_options_description positional; // remains empty

        options_description inputs("Input");
        inputs.add_options()
        ("receptor", value<std::string>(&rigid_name), "rigid part of the receptor (PDBQT)")
        ("flex", value<std::string>(&flex_name), "flexible side chains, if any (PDBQT)")
        ("ligand", value<std::string>(&ligand_name), "ligand (PDBQT)")
        ;
        //options_description search_area("Search area (required, except with --score_only)");
        options_description search_area("Search space (required)");
        search_area.add_options()
        ("center_x", value<fl>(&center_x), "X coordinate of the center")
        ("center_y", value<fl>(&center_y), "Y coordinate of the center")
        ("center_z", value<fl>(&center_z), "Z coordinate of the center")
        ("size_x", value<fl>(&size_x), "size in the X dimension (Angstroms)")
        ("size_y", value<fl>(&size_y), "size in the Y dimension (Angstroms)")
        ("size_z", value<fl>(&size_z), "size in the Z dimension (Angstroms)")
        ;
        //options_description outputs("Output prefixes (optional - by default, input names are stripped of .pdbqt\nare used as prefixes. _001.pdbqt, _002.pdbqt, etc. are appended to the prefixes to produce the output names");
        options_description outputs("Output (optional)");
        outputs.add_options()
        ("out", value<std::string>(&out_name), "output models (PDBQT), the default is chosen based on the ligand file name")
        ("log", value<std::string>(&log_name), "optionally, write log file")
        ;
        options_description advanced("Advanced options (see the manual)");
        advanced.add_options()
        ("score_only",     bool_switch(&score_only),     "score only - search space can be omitted")
        ("local_only",     bool_switch(&local_only),     "do local search only")
        ("randomize_only", bool_switch(&randomize_only), "randomize input, attempting to avoid clashes")
        ("weight_gauss1", value<fl>(&weight_gauss1)->default_value(weight_gauss1),                "gauss_1 weight")
        ("weight_gauss2", value<fl>(&weight_gauss2)->default_value(weight_gauss2),                "gauss_2 weight")
        ("weight_repulsion", value<fl>(&weight_repulsion)->default_value(weight_repulsion),       "repulsion weight")
        ("weight_hydrophobic", value<fl>(&weight_hydrophobic)->default_value(weight_hydrophobic), "hydrophobic weight")
        ("weight_hydrogen", value<fl>(&weight_hydrogen)->default_value(weight_hydrogen),          "Hydrogen bond weight")
        ("weight_rot", value<fl>(&weight_rot)->default_value(weight_rot),                         "N_rot weight")
        ;
        options_description misc("Misc (optional)");
        misc.add_options()
        ("cpu", value<int>(&cpu), "the number of CPUs to use (the default is to try to detect the number of CPUs or, failing that, use 1)")
        ("seed", value<int>(&seed), "explicit random seed")
        ("exhaustiveness", value<int>(&exhaustiveness)->default_value(8), "exhaustiveness of the global search (roughly proportional to time): 1+")
        ("num_modes", value<int>(&num_modes)->default_value(9), "maximum number of binding modes to generate")
        ("energy_range", value<fl>(&energy_range)->default_value(3.0), "maximum energy difference between the best binding mode and the worst one displayed (kcal/mol)")
        ;
        options_description config("Configuration file (optional)");
        config.add_options()
        ("config", value<std::string>(&config_name), "the above options can be put here")
        ;
        options_description info("Information (optional)");
        info.add_options()
        ("help",          bool_switch(&help), "display usage summary")
        ("help_advanced", bool_switch(&help_advanced), "display usage summary with advanced options")
        ("version",       bool_switch(&version), "display program version")
        ;

        options_description batchmodeopt("Batch mode");
        batchmodeopt.add_options()
        ("batch", bool_switch(&batchMode), "Run ligand batches without unloading the receptor.")
        ("jobfile", value<std::string>(&job_file), "job file of ligand path to run")
        ("batchoutdir", value<std::string>(&batch_out), "batch output directory")
        ("fork-parallelism", bool_switch(&use_fork_parallelism), "use fork in addition to per-process threads")
        ("forknbr", value<int>(&forknbr), "number of fork when using fork-based parallelism")
#ifdef SVINA_ENABLE_MPI
        ("mpi", bool_switch(&use_mpi_parallelism), "use OpenMPI-based parallelism (not compatible with forks)")
#endif
        ;
        options_description desc, desc_config, desc_simple;
        desc       .add(inputs).add(search_area).add(outputs).add(advanced).add(misc).add(config).add(batchmodeopt).add(info);
        desc_config.add(inputs).add(search_area).add(outputs).add(advanced).add(batchmodeopt).add(misc);
        desc_simple.add(inputs).add(search_area).add(outputs).add(misc).add(config).add(batchmodeopt).add(info);

        variables_map vm;
        try {
            //store(parse_command_line(argc, argv, desc, command_line_style::default_style ^ command_line_style::allow_guessing), vm);
            store(command_line_parser(argc, argv)
                  .options(desc)
                  .style(command_line_style::default_style ^ command_line_style::allow_guessing)
                  .positional(positional)
                  .run(),
                  vm);
            notify(vm);
        }
        catch(boost::program_options::error& e) {
            std::cerr << "Command line parse error: " << e.what() << '\n' << "\nCorrect usage:\n" << desc_simple << '\n';
            return 1;
        }
        if(vm.count("config")) {
            try {
                path name = make_path(config_name);
                ifile config_stream(name);
                store(parse_config_file(config_stream, desc_config), vm);
                notify(vm);
            }
            catch(boost::program_options::error& e) {
                std::cerr << "Configuration file parse error: " << e.what() << '\n' << "\nCorrect usage:\n" << desc_simple << '\n';
                return 1;
            }
        }
        
        if(vm.count("batch") > 0)
        {
            verbosity = 0;
        }
        
        if(help) {
            std::cout << desc_simple << '\n';
            return 0;
        }
        if(help_advanced) {
            std::cout << desc << '\n';
            return 0;
        }
        if(version) {
            std::cout << version_string << '\n';
            return 0;
        }

        bool search_box_needed = !score_only; // randomize_only and local_only still need the search space
        bool output_produced   = !score_only;
        bool receptor_needed   = !randomize_only;

        if(receptor_needed) {
            if(vm.count("receptor") <= 0) {
                std::cerr << "Missing receptor.\n" << "\nCorrect usage:\n" << desc_simple << '\n';
                return 1;
            }
        }
        if(vm.count("ligand") <= 0 && vm.count("batch") == 0 ) {
            std::cerr << "Missing ligand.\n" << "\nCorrect usage:\n" << desc_simple << '\n';
            return 1;
        }
        if(cpu < 1)
            cpu = 1;
        if(vm.count("seed") == 0)
            seed = auto_seed();
        if(exhaustiveness < 1)
            throw usage_error("exhaustiveness must be 1 or greater");
        if(num_modes < 1)
            throw usage_error("num_modes must be 1 or greater");
        sz max_modes_sz = static_cast<sz>(num_modes);

        boost::optional<std::string> rigid_name_opt;
        if(vm.count("receptor"))
            rigid_name_opt = rigid_name;

        boost::optional<std::string> flex_name_opt;
        if(vm.count("flex"))
            flex_name_opt = flex_name;

        if(vm.count("flex") && !vm.count("receptor"))
            throw usage_error("Flexible side chains are not allowed without the rest of the receptor"); // that's the only way parsing works, actually

        tee log;
        if(vm.count("log") > 0)
            log.init(log_name);

        if(search_box_needed) {
            options_occurrence oo = get_occurrence(vm, search_area);
            if(!oo.all) {
                check_occurrence(vm, search_area);
                std::cerr << "\nCorrect usage:\n" << desc_simple << std::endl;
                return 1;
            }
            if(size_x <= 0 || size_y <= 0 || size_z <= 0)
                throw usage_error("Search space dimensions should be positive");
        }

        if(vm.count("batch") > 0)
        {
            printf("\nBATCH MODE - SVINA\nargv[0] : %s\n\n", argv[0]);
        }else{
        log << cite_message << '\n';
        }
        if(search_box_needed && size_x * size_y * size_z > 27e3) {
            log << "WARNING: The search space volume > 27000 Angstrom^3 (See FAQ)\n";
        }

        if(output_produced) { // FIXME
            if(!vm.count("out")  && batchMode == false) {
                out_name = default_output(ligand_name);
                log << "Output will be " << out_name << '\n';
            }
        }

        grid_dims gd; // n's = 0 via default c'tor

        flv weights;
        weights.push_back(weight_gauss1);
        weights.push_back(weight_gauss2);
        weights.push_back(weight_repulsion);
        weights.push_back(weight_hydrophobic);
        weights.push_back(weight_hydrogen);
        weights.push_back(5 * weight_rot / 0.1 - 1); // linearly maps onto a different range, internally. see everything.cpp

        if(search_box_needed) {
            const fl granularity = 0.375;
            vec span(size_x,   size_y,   size_z);
            vec center(center_x, center_y, center_z);
            VINA_FOR_IN(i, gd) {
                gd[i].n = sz(std::ceil(span[i] / granularity));
                fl real_span = granularity * gd[i].n;
                gd[i].begin = center[i] - real_span/2;
                gd[i].end = gd[i].begin + real_span;
            }
        }
        if(vm.count("cpu") == 0) {
            unsigned num_cpus = boost::thread::hardware_concurrency();
            if(verbosity > 1) {
                if(num_cpus > 0)
                    log << "Detected " << num_cpus << " CPU" << ((num_cpus > 1) ? "s" : "") << '\n';
                else
                    log << "Could not detect the number of CPUs, using 1\n";
            }
            if(num_cpus > 0)
                cpu = num_cpus;
            else
                cpu = 1;
        }
        if(cpu < 1)
            cpu = 1;
        if(verbosity > 1 && exhaustiveness < cpu)
            log << "WARNING: at low exhaustiveness, it may be impossible to utilize all CPUs\n";


        if(batchMode == true && (vm.count("jobfile") == 0 || vm.count("batchoutdir") == 0))
        {
            printf("Batch mode need specification of a job file and output dir\n");
            exit(-1);

        }

        if(batchMode == true && use_mpi_parallelism == false)
        {




            rng a;
            doing(verbosity, "Creating template model", log);

            model templateModel = parse_bundle_partial_screening(*rigid_name_opt); // Create a model without appended ligand.

            done(verbosity,log);

            std::ifstream infile(job_file.c_str());

            int i = 0;
            int maxNbrOfFork = forknbr;
            std::queue<int> pid_queue;
            bool is_a_child_process = false;


            while(true)
            {




                std::string path;
                std::getline(infile, path);

                if (infile.eof() || path == "") {
                    printf("End of file\n");
                    std::cout.flush();
                    while(use_fork_parallelism == true &&  pid_queue.size() != 0)
                    {
                        wait(&(pid_queue.front()));
                        pid_queue.pop();
                    }
                    break;
                }

                // get base filename
                std::string base_filename = path.substr(path.find_last_of("/\\") + 1);
                random_int(1,100000000,a); // REMOVE ?
                seed = random_int(1,100000000,a);


                model* m = new model(templateModel); // Make a copy of the model



                if(use_fork_parallelism == true)
                {
                    pid_t pid = fork();

                    if (pid == 0)
                    {
                        // child process
                        // Continue to the docking procedure
                        is_a_child_process = true;
                    }
                    else if (pid > 0)
                    {
                        pid_queue.push(pid);
                        if(pid_queue.size() >= maxNbrOfFork)
                        {
                            wait(&(pid_queue.front()));
                            pid_queue.pop();
                        }
                        continue;
                    }
                    else
                    {
                        // fork failed
                        printf("\nfork() failed! This should not happen, exiting...\n");
                        return 10;
                    }
                }


                printf("\nDoing ligand number %i (%s)\n",i,base_filename.c_str());
                // Append current ligand
                try {
                    m->append(parse_ligand_pdbqt(make_path(std::vector<std::string>(1, path)[0])));

                    std::string outname = batch_out + "/" + base_filename + ".out.pdbqt";
                    std::cout << "output : " << outname << std::endl;
                    boost::optional<model> ref;



                    main_procedure(*m, ref,
                                   outname.c_str(),
                                   score_only, local_only, randomize_only, false, // no_cache == false
                                   gd, exhaustiveness,
                                   weights,
                                   cpu, seed, verbosity, max_modes_sz, energy_range, log);
                } catch(...)
                {
                    printf("\nException caught, moving on to next ligand...\n");
                    if(is_a_child_process == true)
                    {
                        break;
                    }
                    continue;
                }
                delete m;


                if(is_a_child_process == true)
                {
                    break; // exit after completing task
                }

                i++;
            }
        }
#ifdef SVINA_ENABLE_MPI
        if(batchMode == true && use_mpi_parallelism == true)
        {


            /*
            // Debugging help : wait until you set i != 0 with gdb, and print PID
            {
                int i = 0;
                char hostname[256];
                gethostname(hostname, sizeof(hostname));
                printf("PID %d on %s ready for attach\n", getpid(), hostname);
                fflush(stdout);
                while (0 == i)
                    sleep(5);
            }
            //*/

            // Initialize the MPI environment
            MPI_Init(NULL, NULL);


            // Get the number of processes
            int world_size;
            MPI_Comm_size(MPI_COMM_WORLD, &world_size);


            if(world_size < 2) // Only a governor rank
            {
                puts("Cannot use MPI if there is only one rank available. Use fork-based paralellism, or threads; Exiting...");
                MPI_Finalize();
                return -1;
            }

            /* create a type for struct run_param */
            /*
            const int nitems=3;
            int blocklengths[3] = {1,1,1};
            MPI_Datatype types[3] = {MPI_INT, MPI_INT,MPI_INT};
            MPI_Datatype mpi_run_param;
            MPI_Aint     offsets[3];
            MPI_Aint intex;
            MPI_Type_extent(MPI_INT,&intex);

            offsets[0] = static_cast<MPI_Aint>(0);
            offsets[1] = intex;
            offsets[1] = intex;

            MPI_Type_create_struct(nitems, blocklengths, offsets, types, &mpi_run_param);
            MPI_Type_commit(&mpi_run_param);
            */ // Not using custom type because cant make it work, but would be better FIXME

            //Using contiguous integer instead
            MPI_Datatype mpi_run_param;
            MPI_Type_contiguous(3, MPI_INT, &mpi_run_param);
            MPI_Type_commit(&mpi_run_param);

            bool is_mpi_governor = false;
            bool is_mpi_worker = false;



            // Get the rank of the process
            int rank;
            MPI_Comm_rank(MPI_COMM_WORLD, &rank);
            if (rank == 0) { // Governor process
                is_mpi_governor = true;
            } else { // Worker process
                is_mpi_worker = true;
            }

            // Define stuff common to governor and worker


            const int send_data_tag = 13; // Arbitrary value to tag message
            const int want_data_tag = 13; // Arbitrary value to tag message



            if(is_mpi_governor == true)
            {
                printf("Number of rank : %i\n", world_size);

                rng a;

                std::ifstream infile(job_file.c_str());
                int count = 0;

                std::vector<struct rank_item> rank_list;
                for( int i = 0; i < world_size; i++ )
                {
                    struct rank_item r;
                    r.idx = i;
                    r.r = new int[3];
                    rank_list.push_back(r);
                }

                random_int(1,100000000,a); // REMOVE ? Is it really needed to initialize rng ?

                while(true)
                {
                    int gseed = random_int(1,100000000,a);
                    int seek_offset = (int) infile.tellg();
                    int ligand_nbr = count;

                    // Search for a free worker rank
                    bool found = false;
                    int flag;
                    int recv_processed_counter;
                    MPI_Status status;
                    
                    // Wait for message signaling a worker is ready
                    MPI_Recv(&recv_processed_counter,1, MPI_INT, MPI_ANY_SOURCE, want_data_tag, MPI_COMM_WORLD, &status);

                    int worker_idx = status.MPI_SOURCE;
                    
                    printf("[Governor] RECEIVED request from rank %i (data : %i)\n",
                           worker_idx,
                           recv_processed_counter
                          );
                    
                    rank_list[worker_idx].r[0] = gseed;
                    rank_list[worker_idx].r[1] = seek_offset;
                    rank_list[worker_idx].r[2] = ligand_nbr;
                    

                    MPI_Isend(rank_list[worker_idx].r,1,mpi_run_param,worker_idx,send_data_tag,MPI_COMM_WORLD,&(rank_list[worker_idx].request));
                    printf("[Governor] SENT [%i,%i,%i] to rank %i\n",rank_list[worker_idx].r[0],
                           rank_list[worker_idx].r[1],
                           rank_list[worker_idx].r[0],
                           worker_idx
                          );
                    // Search at which seek index does the next line  lies
                    std::string path;
                    std::getline(infile, path);
                    count++;

                    if (infile.eof() || path == "" || path.find_first_not_of(" \t\n\v\f\r") == std::string::npos) {
                        printf("[Governor] End of batch file\n");
                        std::cout.flush();
                        break; // No more data, exit main loop
                    }

                } // Data-bound while loop

                
                // No data to feed, now wait for worker to finish
                int dummy_data;
                int end_code[3];
                end_code[1] = -1;
                for(int j = 1; j < rank_list.size(); j++)
                {
                    MPI_Recv(&dummy_data,1, MPI_INT, j, want_data_tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE); // Once every worker signals it want data, we're clear
                    MPI_Send(&end_code,1,mpi_run_param,j,send_data_tag,MPI_COMM_WORLD); // send end signal
                    delete rank_list[j].r;
                }
                delete rank_list[0].r; // Not included in previous loop


            } // if(is_mpi_governor == true)

            if(is_mpi_worker == true)
            {
                printf("\nInitializing worker rank %i...\n",rank);

                model templateModel = parse_bundle_partial_screening(*rigid_name_opt); // Create a model without appended ligand.

                std::ifstream infile(job_file.c_str());
                if(infile.is_open() == false)
                {
                    puts("Error : infile not open");
                    return -1;
                }

                const int governor_rank=0; // Receive from governor rank
                int local_processed_counter = 0;
                int recv[3];



                while(true)
                {
                    // Send a message signaling we're ready to process data
                    MPI_Send(&local_processed_counter,1,MPI_INT,governor_rank,want_data_tag,MPI_COMM_WORLD);
                    
                                        
                    // Receive said data.
                    MPI_Recv(&recv,1, mpi_run_param, governor_rank, send_data_tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    
                    if(recv[1] == -1) // Signals ends of processing
                    {
                        break;
                    }
                    
                    model* m = new model(templateModel); // Make a copy of the model



                    infile.seekg(recv[1]);



                    std::string path;
                    std::getline(infile, path);
                    std::string base_filename = path.substr(path.find_last_of("/\\") + 1);


                    printf("[Worker][%i] Received ligand (%i,%s)\n",rank,recv[2],base_filename.c_str());

                    // Append current ligand
                    try {
                        m->append(parse_ligand_pdbqt(make_path(std::vector<std::string>(1, path)[0])));

                        std::string outname = batch_out + "/" + base_filename + ".out.pdbqt";
                        boost::optional<model> ref;

                        main_procedure(*m, ref,
                                       outname.c_str(),
                                       score_only, local_only, randomize_only, false, // no_cache == false
                                       gd, exhaustiveness,
                                       weights,
                                       cpu, recv[0], verbosity, max_modes_sz, energy_range, log);
                    } catch(...)
                    {
                        printf("\nException caught, moving on to next ligand...\n");
                        delete m;
                        local_processed_counter++;
                        continue;
                    }
                    local_processed_counter++;
                    delete m;
                } // Data bound main lopp



            } //  if(is_mpi_worker == true)


            // Finalize the MPI environment.
            MPI_Finalize();

            // MPI : We're done here.


        }
        
#endif // SVINA_ENABLE_MPI

         if(batchMode == false && use_mpi_parallelism == false)
         {

            doing(verbosity, "Reading input", log);



            model m       = parse_bundle(rigid_name_opt, flex_name_opt, std::vector<std::string>(1, ligand_name));

            boost::optional<model> ref;
            done(verbosity, log);



            main_procedure(m, ref,
                           out_name,
                           score_only, local_only, randomize_only, false, // no_cache == false
                           gd, exhaustiveness,
                           weights,
                           cpu, seed, verbosity, max_modes_sz, energy_range, log);

        }
    }
    catch(file_error& e) {
        std::cerr << "\n\nError: could not open \"" << e.name.string() << "\" for " << (e.in ? "reading" : "writing") << ".\n";
        return 1;
    }
    catch(boost::filesystem::filesystem_error& e) {
        std::cerr << "\n\nFile system error: " << e.what() << '\n';
        return 1;
    }
    catch(usage_error& e) {
        std::cerr << "\n\nUsage error: " << e.what() << ".\n";
        return 1;
    }
    catch(parse_error& e) {
        std::cerr << "\n\nParse error on line " << e.line << " in file \"" << e.file.string() << "\": " << e.reason << '\n';
        return 1;
    }
    catch(std::bad_alloc&) {
        std::cerr << "\n\nError: insufficient memory!\n";
        return 1;
    }

    // Errors that shouldn't happen:

    catch(std::exception& e) {
        std::cerr << "\n\nAn error occurred: " << e.what() << ". " << error_message;
        return 1;
    }
    catch(internal_error& e) {
        std::cerr << "\n\nAn internal error occurred in " << e.file << "(" << e.line << "). " << error_message;
        return 1;
    }
    catch(...) {
        std::cerr << "\n\nAn unknown error occurred. " << error_message;
        return 1;
    }
}
