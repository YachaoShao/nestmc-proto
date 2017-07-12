#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <istream>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>

#include <tclap/CmdLine.h>

#include <util/meta.hpp>
#include <util/optional.hpp>
#include <util/strprintf.hpp>

#include "io.hpp"

// Let TCLAP understand value arguments that are of an optional type.

namespace TCLAP {
    template <typename V>
    struct ArgTraits<nest::mc::util::optional<V>> {
        using ValueCategory = ValueLike;
    };
} // namespace TCLAP

namespace nest {
    namespace mc {
        
        namespace util {
            // Using static here because we do not want external linkage for this operator.
            template <typename V>
            static std::istream& operator>>(std::istream& I, optional<V>& v) {
                V u;
                if (I >> u) {
                    v = u;
                }
                return I;
            }
        }
        
        namespace io {
            
            // Override annoying parameters listed back-to-front behaviour.
            //
            // TCLAP argument creation _prepends_ its arguments to the internal
            // list (_argList), where standard options --help etc. are already
            // pre-inserted.
            //
            // reorder_arguments() reverses the arguments to restore ordering,
            // and moves the standard options to the end.
            class CustomCmdLine: public TCLAP::CmdLine {
            public:
                CustomCmdLine(const std::string &message, const std::string &version = "none"):
                TCLAP::CmdLine(message, ' ', version, true)
                {}
                
                void reorder_arguments() {
                    _argList.reverse();
                    for (auto opt: {"help", "version", "ignore_rest"}) {
                        auto i = std::find_if(
                                              _argList.begin(), _argList.end(),
                                              [&opt](TCLAP::Arg* a) { return a->getName()==opt; });
                        
                        if (i!=_argList.end()) {
                            auto a = *i;
                            _argList.erase(i);
                            _argList.push_back(a);
                        }
                    }
                }
            };
            
            // Update an option value from command line argument if set.
            template <
            typename T,
            typename Arg,
            typename = util::enable_if_t<std::is_base_of<TCLAP::Arg, Arg>::value>
            >
            static void update_option(T& opt, Arg& arg) {
                if (arg.isSet()) {
                    opt = arg.getValue();
                }
            }
            
            // Read options from (optional) json file and command line arguments.
            cl_options read_options(int argc, char** argv, bool allow_write) {
                cl_options options;
                std::string save_file = "";
                
                // Parse command line arguments.
                try {
                    cl_options defopts;
                    
                    CustomCmdLine cmd("nest brunel miniapp harness", "0.1");
                    
                    TCLAP::ValueArg<uint32_t> nexc_arg(
                                                         "n", "n_excitatory", "total number of cells in the excitatory population",
                                                         false, defopts.nexc, "integer", cmd);
                    TCLAP::ValueArg<uint32_t> ninh_arg(
                                                            "m", "n_inhibitory", "total number of cells in the inhibitory population",
                                                            false, defopts.ninh, "integer", cmd);
                    TCLAP::ValueArg<double> syn_prop_arg(
                                                       "p", "in_degree_prop", "the proportion of connections from each of the 3 population (excitatory, inhibitory and Poisson) that each neuron receives",
                                                       false, defopts.syn_per_cell_prop, "double", cmd);
                    TCLAP::ValueArg<float> weight_arg(
                                                         "w", "weight", "the weight of all excitatory connections",
                                                         false, defopts.weight, "float", cmd);
                    TCLAP::ValueArg<float> delay_arg(
                                                      "d", "delay", "the delay of all connections",
                                                      false, defopts.delay, "float", cmd);
                    
                    TCLAP::ValueArg<float> rel_inh_strength_arg(
                                                     "g", "rel_inh_w", "relative strength of inhibitory synapses with respect to the excitatory ones",
                                                     false, defopts.rel_inh_strength, "float", cmd);
                    
                    TCLAP::ValueArg<double> poiss_rate_arg(
                                                                "r", "rate", "rate of Poisson cells [kHz]",
                                                                false, defopts.poiss_rate, "double", cmd);

                    TCLAP::ValueArg<double> tfinal_arg(
                                                       "t", "tfinal", "run simulation to <time> ms",
                                                       false, defopts.tfinal, "time", cmd);
                    TCLAP::ValueArg<double> dt_arg(
                                                   "s", "delta_t", "set simulation time step to <time> ms",
                                                   false, defopts.dt, "time", cmd);
            
                    TCLAP::ValueArg<uint32_t> group_size_arg(
                                                             "G", "group-size", "number of cells per cell group",
                                                             false, defopts.group_size, "integer", cmd);
                    TCLAP::SwitchArg spike_output_arg(
                                                      "f","spike-file-output","save spikes to file", cmd, false);
                    TCLAP::ValueArg<unsigned> dry_run_ranks_arg(
                                                                "D","dry-run-ranks","number of ranks in dry run mode",
                                                                false, defopts.dry_run_ranks, "positive integer", cmd);
                    TCLAP::SwitchArg profile_only_zero_arg(
                                                           "z", "profile-only-zero", "Only output profile information for rank 0", cmd, false);
                    TCLAP::SwitchArg verbose_arg(
                                                 "v", "verbose", "Present more verbose information to stdout", cmd, false);
                    cmd.reorder_arguments();
                    cmd.parse(argc, argv);
                    
                    // Handle verbosity separately from other options: it is not considered part
                    // of the saved option state.
                    options.verbose = verbose_arg.getValue();
                    
                    update_option(options.nexc, nexc_arg);
                    update_option(options.ninh, ninh_arg);
                    update_option(options.syn_per_cell_prop, syn_prop_arg);
                    update_option(options.weight, weight_arg);
                    update_option(options.delay, delay_arg);
                    update_option(options.rel_inh_strength, rel_inh_strength_arg);
                    update_option(options.poiss_rate, poiss_rate_arg);
                    update_option(options.tfinal, tfinal_arg);
                    update_option(options.dt, dt_arg);
                    update_option(options.group_size, group_size_arg);
                    update_option(options.spike_file_output, spike_output_arg);
                    update_option(options.profile_only_zero, profile_only_zero_arg);
                    update_option(options.dry_run_ranks, dry_run_ranks_arg);
                    
                    if (options.group_size < 1) {
                        throw usage_error("minimum of one cell per group");
                    }
                    
                    if (options.rel_inh_strength <= 0 || options.rel_inh_strength > 1) {
                        throw usage_error("relative strength of inhibitory connections must be in the interval (0, 1].");
                    }
                }
                catch (TCLAP::ArgException& e) {
                    throw usage_error("error parsing command line argument "+e.argId()+": "+e.error());
                }
                
                // If verbose output requested, emit option summary.
                if (options.verbose) {
                    std::cout << options << "\n";
                }
                
                return options;
            }
            
            std::ostream& operator<<(std::ostream& o, const cl_options& options) {
                o << "simulation options:\n";
                o << "  excitatory cells                                : " << options.nexc << "\n";
                o << "  inhibitory cells                                : " << options.ninh << "\n";
                // Number of Poisson cells is always equal to the number of excitatory cells.
                o << "  Poisson cells (=#excitatory)                    : " << options.nexc << "\n";
                o << "  proportion of synapses/cell from each population: " << options.syn_per_cell_prop << "\n";
                o << "  weight of excitatory synapses                   : " << options.weight << "\n";
                o << "  relative strength of inhibitory synapses        : " << options.rel_inh_strength << "\n";
                o << "  delay of all synapses                           : " << options.delay << "\n";
                o << "  Poisson cells spiking rate [kHz]                : " << options.poiss_rate << "\n";
                o << "\n";
                o << "  simulation time                                 : " << options.tfinal << "\n";
                o << "  dt                                              : " << options.dt << "\n";
                o << "  group size                                      : " << options.group_size << "\n";
                
                return o;
            }
            
        } // namespace io
    } // namespace mc
} // namespace nest
