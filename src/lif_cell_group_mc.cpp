#include <lif_cell_group_mc.hpp>

using namespace nest::mc;

// Constructor containing gid of first cell in a group and a container of all cells.
lif_cell_group_mc::lif_cell_group_mc(cell_gid_type first_gid, const std::vector<util::unique_any>& cells):
gid_base_(first_gid)
{
    output_file = std::ofstream("poisson" + std::to_string(first_gid) + ".txt");

    // reserve
    cells_.reserve(cells.size());

    // resize
    lambda_.resize(cells.size());
    next_poiss_time_.resize(cells.size());
    cell_events_.resize(cells.size());
    last_time_updated_.resize(cells.size());
    poiss_event_counter_ = std::vector<unsigned>(cells.size());

    // Initialize variables for the external Poisson input.
    for (auto lid : util::make_span(0, cells.size())) {
        cells_.push_back(util::any_cast<lif_cell_description>(cells[lid]));

        // If a cell receives some external Poisson input then initialize the corresponding variables.
        if (cells_[lid].n_poiss > 0) {
            EXPECTS(cells_[lid].n_poiss >= 0);
            EXPECTS(cells_[lid].w_poiss >= 0);
            EXPECTS(cells_[lid].d_poiss >= 0);
            EXPECTS(cells_[lid].rate >= 0);
            auto rate = cells_[lid].rate * cells_[lid].n_poiss;
            lambda_[lid] = 1.0 / rate;
            sample_next_poisson(lid);
        }
    }
}

cell_kind lif_cell_group_mc::get_cell_kind() const {
    return cell_kind::lif_neuron;
}

void lif_cell_group_mc::advance(time_type tfinal, time_type dt) {
    PE("lif");
    for (size_t lid = 0; lid < cells_.size(); ++lid) {
        // Advance each cell independently.
        advance_cell(tfinal, dt, lid);
    }
    PL();
}

void lif_cell_group_mc::enqueue_events(const std::vector<postsynaptic_spike_event>& events) {
    if (events.size() == 0) {
        return;
    }

    // sort the events first according to their time and then according to their weight.
    std::vector<postsynaptic_spike_event> sorted_events;
    for (unsigned i = 0; i < events.size(); ++i) {
        sorted_events.push_back(events[i]);
    }

    std::sort(sorted_events.begin(), sorted_events.end(),
        [] (const postsynaptic_spike_event& a, const postsynaptic_spike_event& b) { 
            if (a.time != b.time) {
                return a.time < b.time;
            }
            return a.weight < b.weight;
        }
    );

    // Distribute incoming events to individual cells.
    for (auto& e: sorted_events) {
        cell_events_[e.target.gid - gid_base_].push_back(e);
        // cell_events_[e.target.gid - gid_base_].push(e);
    }
}

const std::vector<spike>& lif_cell_group_mc::spikes() const {
    return spikes_;
}

void lif_cell_group_mc::clear_spikes() {
    spikes_.clear();
}

// TODO: implement sampler
void lif_cell_group_mc::add_sampler(cell_member_type probe_id, sampler_function s, time_type start_time) {}

// TODO: implement binner_
void lif_cell_group_mc::set_binning_policy(binning_kind policy, time_type bin_interval) {
}

// no probes in single-compartment cells
std::vector<probe_record> lif_cell_group_mc::probes() const {
    return {};
}

void lif_cell_group_mc::reset() {
    spikes_.clear();
    // Clear all the event queues.
    for (auto& queue : cell_events_) {
        queue.clear();
    }
    next_poiss_time_.clear();
    poiss_event_counter_.clear();
    last_time_updated_.clear();
}

// Samples next poisson spike.
void lif_cell_group_mc::sample_next_poisson(cell_gid_type lid) {
    // key = GID of the cell
    // counter = total number of Poisson events seen so far
    auto key = gid_base_ + lid + 1225;
    auto counter = poiss_event_counter_[lid];
    ++poiss_event_counter_[lid];

    // First sample unif~Uniform(0, 1) and then use it to get the Poisson distribution
    time_type t_update = random_generator::sample_poisson(lambda_[lid], counter, key);

    next_poiss_time_[lid] += t_update;
}

// Returns the time of the next poisson event for given neuron,
// taking into accout the delay of poisson spikes,
// without sampling a new Poisson event time.
util::optional<time_type> lif_cell_group_mc::next_poisson_event(cell_gid_type lid, time_type tfinal) {
    if (cells_[lid].n_poiss > 0) {
        time_type t_poiss =  next_poiss_time_[lid] + cells_[lid].d_poiss;
        return t_poiss<tfinal ? util::optional<time_type>(t_poiss) : util::nothing;
    }
    return util::nothing;
}

util::optional<postsynaptic_spike_event> pop_if_before(std::vector<postsynaptic_spike_event>& events, time_type tfinal) {
    if (events.size() == 0 || events[0].time >= tfinal) {
        return util::nothing;
    }
    auto ev = events[0];
    events.erase(events.begin());
    return ev;
}

// Returns the next most recent event that is yet to be processed.
// It can be either Poisson event or the queue event.
// Only events that happened before tfinal are considered.
util::optional<postsynaptic_spike_event> lif_cell_group_mc::next_event(cell_gid_type lid, time_type tfinal) {
    if (auto t_poiss = next_poisson_event(lid, tfinal)) {
        // if (auto ev = cell_events_[lid].pop_if_before(std::min(tfinal, t_poiss.get()))) {
        if (auto ev = pop_if_before(cell_events_[lid], tfinal)) {
            return ev;
        }
        sample_next_poisson(lid);
        return postsynaptic_spike_event{{cell_gid_type(gid_base_ + lid), 0}, t_poiss.get(), cells_[lid].w_poiss};
    }

    // t_queue < tfinal < t_poiss => return t_queue
    // return cell_events_[lid].pop_if_before(tfinal);
    return pop_if_before(cell_events_[lid], tfinal);
}

// Advances a single cell (lid) with the exact solution (jumps can be arbitrary).
// Parameter dt is ignored, since we make jumps between two consecutive spikes.
void lif_cell_group_mc::advance_cell(time_type tfinal, time_type dt, cell_gid_type lid) {
    // Current time of last update.
    auto t = last_time_updated_[lid];
    auto& cell = cells_[lid];

    // If a neuron was in the refractory period,
    // ignore any new events that happened before t,
    // including poisson events as well.
    while (auto ev = next_event(lid, t)) {
    };

    // Integrate until tfinal using the exact solution of membrane voltage differential equation.
    while (auto ev = next_event(lid, tfinal)) {
        auto weight = ev->weight;
        auto event_time = ev->time;

        // If a neuron is in refractory period, ignore this event.
        if (event_time < t) {
            continue;
        }

        // Let the membrane potential decay.
        auto decay = exp(-(event_time - t) / cell.tau_m);
        cell.V_m *= decay;
        auto update = weight / cell.C_m;
        // Add jump due to spike.
        cell.V_m += update;
        t = event_time;
        if ( gid_base_ + lid == 0) {
            output_file << event_time << " " << cell.V_m << std::endl;
        }

        // If crossing threshold occurred
        if (cell.V_m >= cell.V_th) {
            cell_member_type spike_neuron_gid = {gid_base_ + lid, 0};
            spike s = {spike_neuron_gid, t};
            spikes_.push_back(s);

            // Advance last_time_updated.
            t += cell.t_ref;

            // Reset the voltage to resting potential.
            cell.V_m = cell.E_L;
        }
        // This is the last time a cell was updated.
        last_time_updated_[lid] = t;
    }
}
