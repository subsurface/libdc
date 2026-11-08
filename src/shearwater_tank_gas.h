/*
 * libdivecomputer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

// AI-generated (Claude)

/* Internal matching of Shearwater air integration transmitters to gas mixes. */
#ifndef SHEARWATER_TANK_GAS_H
#define SHEARWATER_TANK_GAS_H

#define SHEARWATER_TANK_GAS_NONE     0xFFFFFFFF
#define SHEARWATER_TANK_GAS_MAXTANKS 8
#define SHEARWATER_TANK_GAS_MAXGASES 20

/*
 * The logs record the gas mix in use and the transmitter pressures, but not
 * which gas mix each transmitter is on. Assign each transmitter the gas mix
 * it was clearly used most on: the pressure drop while that mix was in use.
 *
 * gas[n] is the gas mix in use at sample n (or SHEARWATER_TANK_GAS_NONE), and
 * pressure[n * ntanks + i] is the reading of transmitter i at sample n (0 if
 * there is none). Readings before a transmitter's highest reading are ignored,
 * so the regulator of a closed stage bleeding down doesn't count as use. The
 * transmitters in the sidemount mask are counted together and get the same
 * gas mix. A transmitter gets a gas mix only if it used at least minuse, and
 * at least twice as much as on any other mix. Otherwise result[i] is
 * SHEARWATER_TANK_GAS_NONE.
 */
static void
shearwater_tank_gas_assign (const unsigned int gas[], const unsigned int pressure[],
	unsigned int nsamples, unsigned int ntanks, unsigned int ngases,
	unsigned int sidemount, int minuse, unsigned int result[])
{
	int use[SHEARWATER_TANK_GAS_MAXTANKS][SHEARWATER_TANK_GAS_MAXGASES] = {{0}};
	const unsigned int stride = ntanks;

	if (ntanks > SHEARWATER_TANK_GAS_MAXTANKS)
		ntanks = SHEARWATER_TANK_GAS_MAXTANKS;
	if (ngases > SHEARWATER_TANK_GAS_MAXGASES)
		ngases = SHEARWATER_TANK_GAS_MAXGASES;

	for (unsigned int i = 0; i < ntanks; ++i) {
		unsigned int highest = 0, start = 0;
		for (unsigned int n = 0; n < nsamples; ++n) {
			if (pressure[n * stride + i] > highest) {
				highest = pressure[n * stride + i];
				start = n;
			}
		}
		if (!highest)
			continue;

		// A drop between two readings only counts if the same gas mix was
		// in use throughout, including the samples without a reading.
		unsigned int previous = highest, previous_gas = gas[start], changed = 0;
		for (unsigned int n = start + 1; n < nsamples; ++n) {
			if (gas[n] != previous_gas)
				changed = 1;
			unsigned int p = pressure[n * stride + i];
			if (p == 0)
				continue;
			if (!changed && gas[n] < ngases)
				use[i][gas[n]] += (int) previous - (int) p;
			previous = p;
			previous_gas = gas[n];
			changed = 0;
		}
	}

	// Sidemount tanks are breathed alternately on one gas mix.
	unsigned int count = 0;
	int pooled[SHEARWATER_TANK_GAS_MAXGASES] = {0};
	for (unsigned int i = 0; i < ntanks; ++i) {
		if (sidemount & (1u << i)) {
			count++;
			for (unsigned int g = 0; g < ngases; ++g)
				pooled[g] += use[i][g];
		}
	}
	if (count >= 2) {
		for (unsigned int i = 0; i < ntanks; ++i) {
			if (sidemount & (1u << i)) {
				for (unsigned int g = 0; g < ngases; ++g)
					use[i][g] = pooled[g];
			}
		}
	}

	for (unsigned int i = 0; i < ntanks; ++i) {
		unsigned int best = 0;
		int second = 0;
		for (unsigned int g = 1; g < ngases; ++g) {
			if (use[i][g] > use[i][best]) {
				if (use[i][best] > second)
					second = use[i][best];
				best = g;
			} else if (use[i][g] > second) {
				second = use[i][g];
			}
		}
		if (ngases && use[i][best] >= minuse && use[i][best] >= 2 * second)
			result[i] = best;
		else
			result[i] = SHEARWATER_TANK_GAS_NONE;
	}
}

#endif
