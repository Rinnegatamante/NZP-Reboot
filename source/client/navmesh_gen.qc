#define NAVGEN_PHASE_FLOOD_FILL						1
#define NAVGEN_PHASE_ZONE_FILL						2
#define NAVGEN_PHASE_RECTANGLE_EXPANSION			3
#define NAVGEN_PHASE_MESH_GENERATION				4
#define NAVGEN_PHASE_MESH_DECIMATION				5
#define NAVGEN_PHASE_MESH_TRIANGULATION				6
#define NAVGEN_PHASE_DONE							7
int cur_navgen_phase;


// ---------------------------------------------------------------------------
// NAVGEN_PHASE_FLOOD_FILL
// ---------------------------------------------------------------------------
void cl_navgen_start_phase_flood_fill();
void cl_navgen_step_phase_flood_fill();
int  cl_navgen_done_phase_flood_fill();
void cl_navgen_draw_phase_flood_fill();
// Cells
struct navgen_cell;
navgen_cell *(int x, int y, int z) create_cell;
void(navgen_cell *cell) delete_cell;
void(navgen_cell *cell, navgen_cell **list_root) add_cell_to_list;
void(navgen_cell *cell, navgen_cell **list_root) add_cell_to_end_of_list;
void(navgen_cell *cell, navgen_cell **list_root) remove_cell_from_list;
navgen_cell *(navgen_cell *cell, navgen_cell **list_root) cell_in_list;
navgen_cell *(int x, int y, int z, navgen_cell **list_root) get_cell_from_list;
// Data
navgen_cell *open_list_root_node;
navgen_cell *closed_list_root_node;
int navgen_cell_size = 8;
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// NAVGEN_PHASE_ZONE_FILL
// ---------------------------------------------------------------------------
void cl_navgen_start_phase_zone_fill();
void cl_navgen_step_phase_zone_fill();
int  cl_navgen_done_phase_zone_fill();
void cl_navgen_draw_phase_zone_fill();
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// NAVGEN_PHASE_RECTANGLE_EXPANSION
// ---------------------------------------------------------------------------
void cl_navgen_start_phase_rectangle_expansion();
void cl_navgen_step_phase_rectangle_expansion();
int  cl_navgen_done_phase_rectangle_expansion();
void cl_navgen_draw_phase_rectangle_expansion();
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// NAVGEN_PHASE_MESH_GENERATION
// ---------------------------------------------------------------------------
void cl_navgen_start_phase_mesh_generation();
void cl_navgen_step_phase_mesh_generation();
int  cl_navgen_done_phase_mesh_generation();
void cl_navgen_draw_phase_mesh_generation();
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// NAVGEN_PHASE_MESH_DECIMATION
// ---------------------------------------------------------------------------
void cl_navgen_start_phase_mesh_decimation();
void cl_navgen_step_phase_mesh_decimation();
int  cl_navgen_done_phase_mesh_decimation();
void cl_navgen_draw_phase_mesh_decimation();
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// NAVGEN_PHASE_MESH_TRIANGULATION
// ---------------------------------------------------------------------------
void cl_navgen_start_phase_mesh_triangulation();
void cl_navgen_step_phase_mesh_triangulation();
int  cl_navgen_done_phase_mesh_triangulation();
void cl_navgen_draw_phase_mesh_triangulation();
// ---------------------------------------------------------------------------

void() navgen_start {
	cur_navgen_phase = NAVGEN_PHASE_FLOOD_FILL;
	cl_navgen_start_phase_flood_fill();
}


void() navgen_logic =
{
	// FIXME - Remove this:
	// ---
	if(TRUE) {
	// ---
	// if(do_navgen_step) {
	// 	print("Stepping navgen algorithm:\n");
	// 	do_navgen_step = FALSE;
	// ---
	if(cur_navgen_phase == NAVGEN_PHASE_FLOOD_FILL) {
		cl_navgen_step_phase_flood_fill();
		if(cl_navgen_done_phase_flood_fill()) {
			cur_navgen_phase = NAVGEN_PHASE_ZONE_FILL;
			cl_navgen_start_phase_zone_fill();
		}
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_ZONE_FILL) {
		cl_navgen_step_phase_zone_fill();
		// if(cl_navgen_done_phase_zone_fill()) {
		// 	cur_navgen_phase = NAVGEN_PHASE_RECTANGLE_EXPANSION;
		// 	cl_navgen_start_phase_rectangle_expansion();
		// }
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_RECTANGLE_EXPANSION) {
		cl_navgen_step_phase_rectangle_expansion();
		if(cl_navgen_done_phase_rectangle_expansion()) {
			cur_navgen_phase = NAVGEN_PHASE_MESH_GENERATION;
			cl_navgen_start_phase_mesh_generation();
		}
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_MESH_GENERATION) {
		cl_navgen_step_phase_mesh_generation();
		if(cl_navgen_done_phase_mesh_generation()) {
			cur_navgen_phase = NAVGEN_PHASE_MESH_DECIMATION;
			cl_navgen_start_phase_mesh_decimation();
		}
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_MESH_DECIMATION) {
		cl_navgen_step_phase_mesh_decimation();
		if(cl_navgen_done_phase_mesh_decimation()) {
			cur_navgen_phase = NAVGEN_PHASE_MESH_TRIANGULATION;
			cl_navgen_start_phase_mesh_triangulation();
		}
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_MESH_TRIANGULATION) {
		cl_navgen_step_phase_mesh_triangulation();
		if(cl_navgen_done_phase_mesh_triangulation()) {
			cur_navgen_phase = NAVGEN_PHASE_DONE;
		}
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_DONE) {
		// TODO 
	}
	}



	if(cur_navgen_phase == NAVGEN_PHASE_FLOOD_FILL) {
		cl_navgen_draw_phase_flood_fill();
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_ZONE_FILL) {
		cl_navgen_draw_phase_zone_fill();
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_RECTANGLE_EXPANSION) {
		cl_navgen_draw_phase_rectangle_expansion();
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_MESH_GENERATION) {
		cl_navgen_draw_phase_mesh_generation();
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_MESH_DECIMATION) {
		cl_navgen_draw_phase_mesh_decimation();
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_MESH_TRIANGULATION) {
		cl_navgen_draw_phase_mesh_triangulation();	
	}
	else if(cur_navgen_phase == NAVGEN_PHASE_DONE) {
		cl_navgen_draw_phase_mesh_triangulation();	
	}
}
// ===========================================================================
// NAVGEN_PHASE_FLOOD_FILL
// ===========================================================================
// TODO - Should implement K-D trees to speed up map rasterization
// Need to implement the following K-D tree functions:
//		insertion
//		deletion
//		search

// ------------------------------------------------------------------------------------------------------
// navgen_cell struct and linked list logic
// ------------------------------------------------------------------------------------------------------
struct navgen_cell {
	int x;
	int y;
	int z;
	// Used to draw edge cells red:
	float is_boundary;
	// Neighbor cells
	navgen_cell *y_next;
	navgen_cell *x_next;
	navgen_cell *y_prev;
	navgen_cell *x_prev;
	// Linked list node
	navgen_cell *next;
	// Cached pointer to the list containing this cell
	navgen_cell **containing_list;
	// Pointer to which zone was created for this cell, if any
	// navgen_zone *zone;
	// Whether or not this cell is the bottom-left corner of a zone
	int is_zone;
	// The rectangle that this cell's zone belongs to (only if is_zone==TRUE)
	// int rectangle_id;
};

navgen_cell *(int x, int y, int z) create_cell {
	navgen_cell *new_cell = memalloc(sizeof(navgen_cell));
	new_cell->x = x;
	new_cell->y = y;
	new_cell->z = z;
	new_cell->y_next = 0;
	new_cell->x_next = 0;
	new_cell->y_prev = 0;
	new_cell->x_prev = 0;
	new_cell->containing_list = 0;
	new_cell->next = 0;
	new_cell->is_zone = FALSE;
	// new_cell->rectangle_id = 0;
	return new_cell;
}


void(navgen_cell *cell) delete_cell {
	memfree(cell);
}


void(navgen_cell *cell, navgen_cell **list_root) add_cell_to_list {
	if(*list_root != 0) {
		cell->next = *list_root;
	}
	*list_root = cell;
	cell->containing_list = list_root;
}


void(navgen_cell *cell, navgen_cell **list_root) add_cell_to_end_of_list {

	if(*list_root == 0) {
		*list_root = cell;
		cell->next = 0;
		cell->containing_list = list_root;
		return;
	}

	navgen_cell *last_cell = *list_root;
	while(last_cell->next != 0) {
		last_cell = last_cell->next;
	}

	last_cell->next = cell;
	cell->next = 0;
	cell->containing_list = list_root;
}


void(navgen_cell *cell, navgen_cell **list_root) remove_cell_from_list {
	if(cell->containing_list != list_root)
		return;
	
	if(cell == *list_root) {
		*list_root = (*list_root)->next;
		cell->next = 0;
		cell->containing_list = 0;
		return;
	}

	navgen_cell *prev_cell = *list_root;
	navgen_cell *cur_cell = (*list_root)->next;

	while(cur_cell != 0) {
		if(cur_cell == cell) {
			prev_cell->next = cur_cell->next;
			cur_cell->next = 0;
			cell->containing_list = 0;
			return;
		}

		prev_cell = cur_cell;
		cur_cell = cur_cell->next;
	}
}

// Checks if 'cell' is in the linked list starting at 'list_root'
// If it is, returns a pointer to cell,
// Otherwise, returns 0
navgen_cell *(navgen_cell *cell, navgen_cell **list_root) cell_in_list {
	if(cell->containing_list == list_root) {
		return cell;
	}
	return 0;
}



// TODO - This method is prime for optimization.
// TODO - There's gotta' be a better way than to just linear-search
// TODO   through the entire linked list.
// TODO   A hash-based lookup would work, but might be tricky to implement.
navgen_cell *(int x, int y, int z, navgen_cell **list_root) get_cell_from_list {
	navgen_cell *cur_cell = *list_root;

	int iters = 0;

	while(cur_cell != 0) {
		// HACK HACK HACK HACK HACK HACK 
		// Easy hack to bypass 10k runaway loop error
		// HACK HACK HACK HACK HACK HACK 
		if(iters > 9000) {
			return get_cell_from_list(x,y,z,&cur_cell);
		}
		iters++;
		// HACK HACK HACK HACK HACK HACK 
		if(cur_cell->x == x && cur_cell->y == y && cur_cell->z == z) {
			return cur_cell;
		}
		cur_cell = cur_cell->next;
	}
	return 0;
}

void mark_cells_as_neighbors(navgen_cell *a, navgen_cell *b, int ofs_x, int ofs_y) {
	// Assume that (ofs_x,ofs_y) takes us from a to b
	if(ofs_x == 0 && ofs_y == 1) {
		a->y_next = b;
		b->y_prev = a;
	}
	else if(ofs_x == 1 && ofs_y == 0) {
		a->x_next = b;
		b->x_prev = a;
	}
	else if(ofs_x == 0 && ofs_y == -1) {
		a->y_prev = b;
		b->y_next = a;
	}
	else if(ofs_x == -1 && ofs_y == 0) {
		a->x_prev = b;
		b->x_next = a;
	}
}

vector get_cell_pos(navgen_cell *cell) {
	return [cell->x, cell->y, cell->z];
}
// ------------------------------------------------------------------------------------------------------

void cl_navgen_start_phase_flood_fill() {
	// If the lists have any previous nodes, delete them all
	navgen_cell *temp_cell;
	navgen_cell *temp_cell_next;

	// Empty the open list
	temp_cell = open_list_root_node;
	while(temp_cell != 0)
	{
		temp_cell_next = temp_cell->next;
		remove_cell_from_list(temp_cell, &open_list_root_node);
		delete_cell(temp_cell);
		temp_cell = temp_cell_next;
	}

	// Empty the closed list
	temp_cell = closed_list_root_node;
	while(temp_cell != 0)
	{
		temp_cell_next = temp_cell->next;
		remove_cell_from_list(temp_cell, &closed_list_root_node);
		temp_cell = temp_cell_next;
	}

	// Create a cell at the player's position:
	vector player_pos = getentity(player_localentnum, GE_ORIGIN);
	// Make sure it's at the floor
	navgen_drop_to_floor(player_pos);
	player_pos = trace_endpos;

	int pos_x = floor(player_pos.x);
	int pos_y = floor(player_pos.y);
	// Lift up above the floor
	int pos_z = ceil(player_pos.z);

	navgen_cell *cell = create_cell(pos_x, pos_y, pos_z);

	// Add it to the open list
	add_cell_to_list(cell, &open_list_root_node);
}


void cl_navgen_step_phase_flood_fill() {
	int verbose = FALSE;
	if(verbose) print("---- Navgen logic tick start ----\n");

	navgen_cell *cell = open_list_root_node;

	// One entry for each of the 8 cardinal + intercardinal directions
	// int num_ofs = 8;
	// int ofs_x_vals[8] = { 0, 1, 1, 1, 0,-1,-1,-1};
	// int ofs_y_vals[8] = { 1, 1, 0,-1,-1,-1, 0, 1};

	// One entry for each of the 4 cardinal directions
	int num_ofs = 4;
	int ofs_x_vals[4] = { 0, 1, 0,-1};
	int ofs_y_vals[4] = { 1, 0,-1, 0};

	// How many cells to process this frame
	int cells_to_process_this_frame = 100;

	while(cell != 0)
	{
		if(verbose) print("Looping through cell, cells remaining: ", itos(cells_to_process_this_frame) ,"\n");
		cells_to_process_this_frame -= 1;
		if(cells_to_process_this_frame < 0) {
			return;
		}

		if(verbose) print("\t-- Handling cell at: (",ftos(cell->x),",",ftos(cell->y),",",ftos(cell->z),") --\n");

		// For each of the directions:
		for(int i = 0; i < num_ofs; i++) {
			if(verbose) print("Looping through ofs: ", itos(i) ,"\n");

			int ofs_x = ofs_x_vals[i] * navgen_cell_size;
			int ofs_y = ofs_y_vals[i] * navgen_cell_size;
			int next_cell_x = cell->x + ofs_x;
			int next_cell_y = cell->y + ofs_y;
			
			// If we can't fully walk from cell to next_cell, this is a boundary
			vector cell_pos;
			cell_pos.x = cell->x;
			cell_pos.y = cell->y;
			cell_pos.z = cell->z;

			vector next_cell_pos;
			next_cell_pos.x = next_cell_x;
			next_cell_pos.y = next_cell_y;
			next_cell_pos.z = cell->z;

			int obstructed = navgen_obstructed(cell_pos, next_cell_pos);

			// Lift up above the floor, snap to grid of size 'navgen_cell_size'
			int z_pos = cell->z + navgen_cell_size * floor((ceil(trace_endpos.z) - cell->z) / navgen_cell_size);

			// If we can't walk to a neighbor, mark this cell as a boundary
			if(obstructed) {
				if(verbose) print("\t\tCell was obstructed, logging boundary.\n");
				cell->is_boundary = TRUE;
			}
			// If we can walk to neighbor, add neighbor to the open list
			else {
				navgen_cell *neighbor;

				// If this cell is already in the closed list, skip it
				neighbor = get_cell_from_list(next_cell_x, next_cell_y, z_pos, &closed_list_root_node);
				if(neighbor != 0) {
					mark_cells_as_neighbors(cell, neighbor, ofs_x_vals[i], ofs_y_vals[i]);
					if(verbose) print("\t\tCell is in closed list, skipping...\n");
					continue;
				}

				// If this cell is already in the open list, skip it
				neighbor = get_cell_from_list(next_cell_x, next_cell_y, z_pos, &open_list_root_node);
				if(neighbor != 0) {
					mark_cells_as_neighbors(cell, neighbor, ofs_x_vals[i], ofs_y_vals[i]);
					if(verbose) print("\t\tCell is in open list, skipping...\n");
					continue;
				}

				if(verbose) print("\t\tCell is not in any list, adding new cell to open list.\n");
				navgen_cell *new_cell = create_cell(next_cell_x, next_cell_y, z_pos);
				mark_cells_as_neighbors(cell, new_cell, ofs_x_vals[i], ofs_y_vals[i]);
				add_cell_to_end_of_list(new_cell, &open_list_root_node);
			}
		}

		// Remove cell from the current open list
		if(verbose) print("\t\tRemove it from the open list\n");
		remove_cell_from_list(cell, &open_list_root_node);

		// Add cell to closed list
		if(verbose) print("\t\tAdding cell to closed list.\n");
		add_cell_to_list(cell, &closed_list_root_node);

		// Get the next cell
		cell = open_list_root_node;
	}
}


int  cl_navgen_done_phase_flood_fill() {
	return open_list_root_node == 0;
}


// Returns TRUE if vectors are within dist of each other (manhatten distance is used for speed)
// Returns FALSE otherwise.
int vecs_nearby(vector a, vector b, float dist) {
	if(b.x < a.x - dist || b.y < a.y - dist || b.z < a.z - dist) {
		return FALSE;
	}
	if(b.x > a.x + dist || b.y > a.y + dist || b.z > a.z + dist) {
		return FALSE;
	}
	return TRUE;
}


void cl_navgen_draw_phase_flood_fill() {
	vector open_cell_color = [0,0,1];
	vector closed_cell_color = [0,1,0];
	vector boundary_color = [1,0,0];

	float open_cell_alpha = 0.2;
	float closed_cell_alpha = 0.01;
	float boundary_alpha = 0.4;

	navgen_cell *cell;

	vector player_pos = getentity(player_localentnum, GE_ORIGIN);
	float draw_dist = 300;

	// ------------------------------------------------------------------------
	// Draw the open list
	// ------------------------------------------------------------------------
	cell = open_list_root_node;

	while(cell != 0)
	{
		cl_navmesh_draw_vert(get_cell_pos(cell),open_cell_color,open_cell_alpha);
		cell = cell->next;
	}

	// ------------------------------------------------------------------------
	// Draw the closed list
	// ------------------------------------------------------------------------
	// NOTE - The closed list of cells is MASSIVE, this kills FPS.
	cell = closed_list_root_node;

	while(cell != 0)
	{
		if(cell->is_boundary) {
			if(vecs_nearby(get_cell_pos(cell), player_pos, draw_dist)) {
				cl_navmesh_draw_vert([cell->x, cell->y, cell->z],boundary_color,boundary_alpha);
			}
		}

	// 	// Draw the neighbors:
	// 	vector pos = [cell->x, cell->y, cell->z];
	// 	// Offset start pos by -8, to show direction
	// 	pos.z -= 8;
	// 	vector neighbor_pos;
	// 	navgen_cell *neighbor;

	// 	if(cell->y_next != 0)
	// 	{
	// 		neighbor = cell->y_next;
	// 		neighbor_pos = [neighbor->x, neighbor->y, neighbor->z];
	// 		cl_navmesh_draw_edge(pos, neighbor_pos);
	// 	}
	// 	if(cell->x_next != 0)
	// 	{
	// 		neighbor = cell->x_next;
	// 		neighbor_pos = [neighbor->x, neighbor->y, neighbor->z];
	// 		cl_navmesh_draw_edge(pos, neighbor_pos);
	// 	}
	// 	if(cell->y_prev != 0)
	// 	{
	// 		neighbor = cell->y_prev;
	// 		neighbor_pos = [neighbor->x, neighbor->y, neighbor->z];
	// 		cl_navmesh_draw_edge(pos, neighbor_pos);
	// 	}
	// 	if(cell->x_prev != 0)
	// 	{
	// 		neighbor = cell->x_prev;
	// 		neighbor_pos = [neighbor->x, neighbor->y, neighbor->z];
	// 		cl_navmesh_draw_edge(pos, neighbor_pos);
	// 	}
		cell = cell->next;
	}

	// ------------------------------------------------------------------------
	// Draw the boundaries
	// ------------------------------------------------------------------------
	// navgen_boundary *boundary = boundary_list_root_node;
	// while(boundary != 0)
	// {
	// 	cl_navmesh_draw_vert([boundary->x, boundary->y, boundary->z],boundary_color,boundary_alpha);
	// 	boundary = boundary->next;
	// }
}

// ===========================================================================
// ===========================================================================
// NAVGEN_PHASE_ZONE_FILL
// ===========================================================================
// Data
navgen_cell *zone_list_root_node;


// Returns TRUE if cell is the bottom-left corner of a fully-connected neighborhood
// Returns FALSE otherwise.
int is_cell_fully_connected(navgen_cell *bl_cell) {
	if(bl_cell->x_next == 0 || bl_cell->y_next == 0) {
		return FALSE;
	}
	navgen_cell *br_cell = bl_cell->x_next;
	navgen_cell *tl_cell = bl_cell->y_next;
	if(br_cell->x_prev != bl_cell || tl_cell->y_prev != bl_cell) {
		return FALSE;
	}
	if(br_cell->y_next == 0 || tl_cell->x_next == 0) {
		return FALSE;
	}
	navgen_cell *tr_cell = tl_cell->x_next;
	if(br_cell->y_next != tr_cell) {
		return FALSE;
	}
	if(tr_cell->x_prev != tl_cell || tr_cell->y_prev != br_cell) {
		return FALSE;
	}
	return TRUE;
}


void cl_navgen_start_phase_zone_fill() {
	// Empty the zone list
	navgen_cell *temp_cell = zone_list_root_node;
	navgen_cell *temp_cell_next;

	while(temp_cell != 0)
	{
		temp_cell_next = temp_cell->next;
		remove_cell_from_list(temp_cell, &zone_list_root_node);
		delete_cell(temp_cell);
		temp_cell = temp_cell_next;
	}
}

void cl_navgen_step_phase_zone_fill() {

	int cells_to_process_this_frame = 1000;

	// Process all cells in the closed list and check which are zones
	navgen_cell *cell = closed_list_root_node;

	while(cell != 0) {
		cells_to_process_this_frame -= 1;
		if(cells_to_process_this_frame < 0) {
			return;
		}
		remove_cell_from_list(cell, &closed_list_root_node);
		// Check if cell is the bottom-left corner of a fully-connected 4 cell
		// neighborhood. We will call these regions "zones"
		if(is_cell_fully_connected(cell)) {
			cell->is_zone = TRUE;
			// Add it to the list of zones
			add_cell_to_list(cell, &zone_list_root_node);
		}
		cell = closed_list_root_node;
	}
}
int  cl_navgen_done_phase_zone_fill() {
	return closed_list_root_node == 0;
}
void cl_navgen_draw_phase_zone_fill() {

	vector zone_color = [0.8,0.2,0.2];	
	float zone_alpha = 0.2;
	// Offset the zone corners by 1 QU to show they're distinct polygons
	float ofs = 1;
	navgen_cell *cell = zone_list_root_node;

	R_BeginPolygon("debug/wireframe",0);

	vector player_pos = getentity(player_localentnum, GE_ORIGIN);
	float draw_dist = 300;

	while(cell != 0)
	{
		if(cell->is_zone) {
			if(vecs_nearby(get_cell_pos(cell), player_pos, draw_dist)) {
				vector bl_corner = get_cell_pos(cell) + [ofs,ofs,0];
				vector br_corner = get_cell_pos(cell->x_next) + [-ofs,ofs,0];
				vector tr_corner = get_cell_pos((cell->x_next)->y_next) + [-ofs,-ofs,0];
				vector tl_corner = get_cell_pos(cell->y_next) + [ofs,-ofs,0];

				R_BeginPolygon("debug/solid_nocull",0);
				
				//Draw an edge connecting portals
				R_PolygonVertex(bl_corner, [0,0,0], zone_color, zone_alpha);
				R_PolygonVertex(br_corner, [0,0,0], zone_color, zone_alpha);
				R_PolygonVertex(tr_corner, [0,0,0], zone_color, zone_alpha);
				R_PolygonVertex(tl_corner, [0,0,0], zone_color, zone_alpha);
				R_EndPolygon();
			}
		}
		cell = cell->next;
	}
}
// ===========================================================================
// ===========================================================================
// NAVGEN_PHASE_RECTANGLE_EXPANSION
// ===========================================================================
void cl_navgen_start_phase_rectangle_expansion() {

}
void cl_navgen_step_phase_rectangle_expansion() {

}
int  cl_navgen_done_phase_rectangle_expansion() {
	return 1;
}
void cl_navgen_draw_phase_rectangle_expansion() {

}
// ===========================================================================
// ===========================================================================
// NAVGEN_PHASE_MESH_GENERATION
// ===========================================================================
void cl_navgen_start_phase_mesh_generation() {

}
void cl_navgen_step_phase_mesh_generation() {

}
int  cl_navgen_done_phase_mesh_generation() {
	return 1;
}
void cl_navgen_draw_phase_mesh_generation() {

}
// ===========================================================================
// ===========================================================================
// NAVGEN_PHASE_MESH_DECIMATION
// ===========================================================================
void cl_navgen_start_phase_mesh_decimation() {

}
void cl_navgen_step_phase_mesh_decimation() {

}
int  cl_navgen_done_phase_mesh_decimation() {
	return 1;
}
void cl_navgen_draw_phase_mesh_decimation() {

}
// ===========================================================================
// ===========================================================================
// NAVGEN_PHASE_MESH_TRIANGULATION
// ===========================================================================
void cl_navgen_start_phase_mesh_triangulation() {

}
void cl_navgen_step_phase_mesh_triangulation() {

}
int  cl_navgen_done_phase_mesh_triangulation() {
	return 1;
}
void cl_navgen_draw_phase_mesh_triangulation() {
	
}
// ===========================================================================





// ===========================================================================
// Everything below this is old stuff:
// ===========================================================================

