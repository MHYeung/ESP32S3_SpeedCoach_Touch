#include <stdio.h>
#include "tcx_export.h

// start chrono
void tcx_write_header(FILE* file, const char* start_time_iso) {
    if (file == NULL) return;

    fprintf(file, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(file, "<TrainingCenterDatabase xmlns=\"http://www.garmin.com/xmlschemas/TrainingCenterDatabase/v2\">\n");
    fprintf(file, "  <Activities>\n");
    fprintf(file, "    <Activity Sport=\"Rowing\">\n"); // Sport "Rowing" ou "Other"
    fprintf(file, "      <Id>%s</Id>\n", start_time_iso);
    fprintf(file, "      <Lap StartTime=\"%s\">\n", start_time_iso);
    fprintf(file, "        <Intensity>Active</Intensity>\n");
    fprintf(file, "        <Track>\n");
}

// function to call to create file(ex: à 1Hz)
void tcx_append_trackpoint(FILE* file, const char* current_time_iso, double lat, double lon, double altitude, int bpm) {
    if (file == NULL) return;

    fprintf(file, "          <Trackpoint>\n");
    fprintf(file, "            <Time>%s</Time>\n", current_time_iso);
    
    // GPS
    fprintf(file, "            <Position>\n");
    fprintf(file, "              <LatitudeDegrees>%.6f</LatitudeDegrees>\n", lat);
    fprintf(file, "              <LongitudeDegrees>%.6f</LongitudeDegrees>\n", lon);
    fprintf(file, "            </Position>\n");
    
    // elevation (better for the file but not necessary for rowing)
    fprintf(file, "            <AltitudeMeters>%.1f</AltitudeMeters>\n", altitude);
    
    // BPM (When BPM)
    if (bpm > 0) {
        fprintf(file, "            <HeartRateBpm>\n");
        fprintf(file, "              <Value>%d</Value>\n", bpm);
        fprintf(file, "            </HeartRateBpm>\n");
    }
    
    fprintf(file, "          </Trackpoint>\n");
    
    // write file when prblm
    fflush(file); 
}

// Stop Chrono
void tcx_close_file(FILE* file) {
    if (file == NULL) return;

    fprintf(file, "        </Track>\n");
    fprintf(file, "      </Lap>\n");
    fprintf(file, "    </Activity>\n");
    fprintf(file, "  </Activities>\n");
    fprintf(file, "</TrainingCenterDatabase>\n");
    
    fclose(file);
}