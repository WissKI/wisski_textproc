<?php


/** @see wisski_textproc_analyse_xhtml_fragment()
*
* @author Martin Scholz
*
*/
function _wisski_textproc_analyse_xhtml_fragment($xhtml, $default_lang = '', $profile = 'default') {

  $overlap_factor = 0.005;
  if ($default_lang == '') $default_lang = variable_get('wisski_textproc_default_document_lang', 'de');

  $text_struct = wisski_textproc_strip_tags($xhtml, $default_lang);

  $langs = array();
  foreach ($text_struct['lang_ranges'] as $lang => $ranges) {
    foreach ($ranges as $range) {
      $langs[$range[0]] = array($lang, $range[0], $range[1]);
    }
  }
  ksort($langs);
  $char_shifts = $text_struct['char_shifts'];

  $annos = $text_struct['annos'];
  foreach ($langs as $range) {
    list($lang, $start, $end) = $range;
    $text = mb_substr($text_struct['text'], $start, $end - $start);
    $tokens = wisski_textproc_tokenize($text, $lang);
    for ($i = count($tokens) - 1; $i >= 0; $i--) {
      $tokens[$i][1] += $start;
      $tokens[$i][2] += $start;
    }
    
    $tokens = wisski_textproc_preprocess($text, $tokens, $lang);
    
    $res = db_query("SELECT * FROM {wisski_textproc_pipes} WHERE profile = '%s' AND '%s' LIKE CONCAT(lang, '%%')", $profile, $lang);
    $pipe = array();
    while ($step = db_fetch_object($res)) $pipe[] = $step;
    usort($pipe, function($a, $b) { return $b->weight - $a->weight; });

    foreach ($pipe as $step) {
      $tlog = time();
      $annos_ret = module_invoke($step->module, 'wisski_textproc', 'analyse', $step->type, $profile, $lang, $text, $tokens, $annos, unserialize($step->settings));

      $annos = array_merge($annos, $annos_ret);
    }   
  }

  // annos without uri affect rank of overlapping uris of same class
  // also, a new uri is added to such annos
  foreach ($annos as $k1 => $a1) {
    if (isset($a1['uri'])) continue;
    foreach ($annos as $k2 => $a2) {
      if ($k1 == $k2) continue;
      if ($a1['range'][0] >= $a2['range'][1] || $a1['range'][1] <= $a2['range'][0]) continue; // no overlap
      if ($a1['class'] != $a2['class']) continue;
      $overlap = ($a1['range'][0] < $a2['range'][0]) ? 
        (($a1['range'][1] < $a2['range'][1]) ? $a1['range'][1] - $a2['range'][0]
         : $a2['range'][1] - $a2['range'][0])
        :
        (($a1['range'][1] < $a2['range'][1]) ? $a1['range'][1] - $a1['range'][0]
         : $a2['range'][1] - $a1['range'][0]);
      $annos[$k1]['rank'] += $overlap_factor * $a1['rank'] * $overlap;
    }
  }
  foreach ($annos as $k1 => $a1) {
    if (isset($a1['uri'])) continue;
    $annos[$k1]['uri'] = wisski_instance_prefix() . wisski_get_uuid(4) . "_" . $annos[$k1]['class'];
  }


  // sort out annos under a certain threshold
  $threshold = 1;
  foreach ($annos as $k => $a) {
    if (!$a['approved'] && $a['rank'] < $threshold) unset($annos[$k]);
  }

  // only select top ranked annotation
  // sort by priority (first) and length (second)
  usort($annos, '_wisski_textproc_compare_annotations');
  $annos = array_reverse($annos);

  // select the top set of annos that don't overlap
  $top_annos = array();
  $li = count($annos);
  for ($i = 0; $i < $li; $i++) {
    $candidate = $annos[$i];
    $top = TRUE;
    $lj = count($top_annos);
    for ($j = 0; $j < $lj; $j++) {
      $a = $top_annos[$j];
      if ($candidate['range'][1] > $a['range'][0] && $candidate['range'][0] < $a['range'][1]) {
        // annotations overlap => cannot be top annotation
        $top = FALSE;
        break;
      }
    }
    if ($top) {
      //annotation did not overlap with any other top annotation => add to top annos
      $top_annos[$lj] = $candidate;
    }

  }

  $top_annos = wisski_textproc_link_events($profile, $lang, $text, $tokens, $top_annos, array('db_name' => 'wisski_textproc_events', 'pos_mapping' => array('n' => array('NE', 'NN'), 'v' => array('VVFIN', 'VVINF', 'VVIMP', 'VVPP'))));
  
  // adjust ranges
  foreach ($top_annos as $k => &$anno) {
    $s = $anno['range'][0]; // + $start;
    $e = $anno['range'][1]; // + $start;
    if ($s == 0 && $e == 0) continue; // pos 0 always shifts back to 0!
    foreach ($char_shifts as $shift => $offset) {
      if ($s != 0 && $s < $offset) {
        $anno['range'][0] = $s - $shift;
        $s = 0;
      }
      if ($e < $offset) {
        $anno['range'][1] = $e - $shift;
        break;
      }
    }
  }

  return $top_annos;

}


/* Call an external preprocessor for lemma and part-of-speech
*
* @return token array where tokens are enhanced with keys 'lemma' and 'pos'
* where information was available
*
* @author Martin Scholz
*
*/
function wisski_textproc_preprocess($text, $tokens, $lang) {

  $proc = variable_get('wisski_textproc_preprocessor', array('enabled' => FALSE));
  if (!$proc['enabled']) return $tokens;
  if (!$proc[$lang] || !$proc[$lang]['cmd']) {
    return $tokens;
  }
  $proc = $proc[$lang];

  $str = "";
  foreach ($tokens as $t) $str .= "\n" . mb_substr($text, $t[1], $t[2] - $t[1]);
  if (!$str) return $tokens;
  $str = substr($str, 1) . "\n";

  $descriptorspec = array(
      0 => array("pipe", "r"),  // STDIN ist eine Pipe, von der das Child liest
      1 => array("pipe", "w"),  // STDOUT ist eine Pipe, in die das Child schreibt
      2 => array("pipe", "a"),
      );

  $proc = proc_open($proc['cmd'], $descriptorspec, $pipes, $proc['cwd']);
  if (!is_resource($proc)) {
    drupal_set_message("Could not call preprocessor.", 'error');
    return $tokens;
  }

  $err = stream_get_contents($pipes[2]);
  if ($err) {
  }


  fwrite($pipes[0], $str);
  fclose($pipes[0]);

  $lines = stream_get_contents($pipes[1]);
  fclose($pipes[1]);
  proc_close($proc);

  $new_tokens = $tokens;
  $lines = explode("\n", $lines);
  $i = 0;
  foreach($lines as $line) {
    $line = rtrim($line);
    if (empty($line)) continue;  // only whitespace, skip
    $line = explode("\t", $line);  // should be array(<lemma>, <pos>?, <something else...>?)
    if (isset($line[0]) && !empty($line[0])) $new_tokens[$i]['lemma'] = $line[0];
    if (isset($line[1]) && !empty($line[1])) $new_tokens[$i]['pos'] = $line[1];
    $i++;
  }

wisski_log($new_tokens);

  return $new_tokens;  

}



/** Compares 2 annotations
*
* @author Martin Scholz
*
*/
function _wisski_textproc_compare_annotations($a, $b) {

// approved anno always beats non-approved anno
  if (isset($a['approved']) && $a['approved']) {
    if (!isset($b['approved']) || !$b['approved']) return 1;
  } elseif (isset($b['approved']) && $b['approved']) {
    return -1;
  } 

  // if both are not approved, the rank decides. (Also if both are approved, though this should not occur)
  if (isset($a['rank']) && isset($b['rank'])) {

    // the higher ranked anno is greater
    $c = $a['rank'] - $b['rank'];
    if ($c) return $c;
    // if rank is equal, the longer anno is greater
    $l = ($a['range'][1] - $a['range'][0]) - ($b['range'][1] - $b['range'][0]);
    if ($l) return $l;
    // if length is equal, the one that has an 'approved' key is greater (it surely comes from the editor, thus we prefer the 'old' anno)
    return (isset($b['approved'])) ? -1 : 1;

  } elseif (!isset($a['rank'])) {

    // annotation without rank is considered higher, because:
    // no rank generally means that the annotation comes from outside
    return -1;

  } else {

    return -1;  // $b['rank'] not set

  }

}


/** Find occurences of vocabulary terms in text
* This is the analyse operation for analysis component type vocab
*
* @author Martin Scholz, Florian Wittmann
*
*/
function _wisski_textproc_analyse_vocab($profile, $lang, $text, $tokens, $annos, $settings) {

  if(!isset($settings['rank_offset_exact'])) $settings['rank_offset_exact'] = 1;
  if(!isset($settings['rank_factor_length_contains'])) $settings['rank_factor_length_contains'] = 5;
  if(!isset($settings['rank_offset_contains'])) $settings['rank_offset_contains'] = 1;
  if(!isset($settings['rank_factor_length_guess'])) $settings['rank_factor_length_guess'] = -2.4;
  if(!isset($settings['rank_offset_guess'])) $settings['rank_offset_guess'] = -1;
  if(!isset($settings['rank_offset_coords_lat'])) $settings['rank_offset_coords_lat'] = -1.5;
  if(!isset($settings['rank_offset_coords_long'])) $settings['rank_offset_coords_long'] = -2.4;
  if(!isset($settings['rank_offset_has_lemma'])) $settings['rank_offset_has_lemma'] = array('62' => -6.5);
  if(!isset($settings['rank_offset_has_pos'])) $settings['rank_offset_has_pos'] = array('62' => array('NE' => 0,'NN' => 0, '' => -6));

  if (isset($settings['place_use_approved_coordinates']) && $settings['place_use_approved_coordinates']) {
    foreach ($annos as $anno) {
      if (isset($anno['approved']) && $anno['approved'] &&
          isset($anno['uri']) && isset($anno['voc']) && in_array($anno['class'], $settings['place_classes'])) {
        $c = wisski_vocab_ctrl_lookup(array('latitude', 'longitude'), $anno['voc'], $anno['uri'], NULL);
        $settings['place_coords'][] = array('lat' => $c[$anno['voc']][$anno['uri']]['latitude'][0]['value'], 'long' => $c[$anno['voc']][$anno['uri']]['longitude'][0]['value']);
      }
    }
  }

  $annos = array(); // don't care about old annotations

  $tokens_len = count($tokens);
  for ($token_pos = 0; $token_pos < $tokens_len; $token_pos++) {

    $tmp =  _wisski_textproc_analyse_vocab_token($tokens, $token_pos, $tokens_len, $annos, $settings);

    $annos = array_merge($annos, $tmp);
  }

  return $annos;

}


/* Helper function for wisski_textproc_analyse_vocab
* Analyse a single token
*
* @author Martin Scholz, Florian Wittmann
*
*/
function _wisski_textproc_analyse_vocab_token($tokens, $token_pos, $tokens_len, $annos, $settings) {
  $rank_offset_exact = $settings['rank_offset_exact'];
  $rank_factor_length_contains = $settings['rank_factor_length_contains'];
  $rank_offset_contains = $settings['rank_offset_contains'];
  $rank_factor_length_guess = $settings['rank_factor_length_guess'];
  $rank_offset_guess = $settings['rank_offset_guess'];
  $rank_offset_coords_lat = $settings['rank_offset_coords_lat'];
  $rank_offset_coords_long = $settings['rank_offset_coords_long'];
  $coords = $settings['place_coords'];
  $place_classes = $settings['place_classes'];
  $rank_offset_has_lemma = $settings['rank_offset_has_lemma'];
  $rank_offset_has_pos = $settings['rank_offset_has_pos'];

  $token = $tokens[$token_pos];

  // first perform an exact query, then a broader one
  $exact = array();
  $overlap = array();
  $guess = array();

  // this is the exact query; 
  $q  = "SELECT * FROM {wisski_vocab_ctrl_index} WHERE value = '%s'";
  $res = db_query($q, $token[0]);
  while ($row = db_fetch_object($res)) {

    $label = $row->value;
    if ($label != $token[0]) continue;
    $voc = wisski_vocab_ctrl_get($row->vid);


    $constraint_offset = _wisski_textproc_analyse_coord_offset($row, $voc, $settings);
    //check for lemma
    $constraint_offset += _wisski_textproc_analyse_lemma_offset($token, $voc, $settings);
    //check for pos
    $constraint_offset += _wisski_textproc_analyse_pos_offset($token, $voc, $settings);

    $exact[] = array(
        'voc' => $voc->vid,
        'class' => (string) $voc->group_id,
        'uri' => $row->uri,
        'label' => $label,
        'lang' => $row->lang,
        'range' => array($token[1], $token[2]),
        'rank' => $voc->priority + $rank_offset_exact + $constraint_offset,
        );

  }

  if ($exact) {
    usort($exact, function($a, $b) { $c = $b['rank'] - $a['rank']; return ($c == 0) ? 0 : ($c < 0) ? -1 : 1; });
    $exact = array(wisski_vocab_ctrl_get($exact[0]['voc'], 'priority') => array($exact[0]));
  }

  // this is the 'contains_word' match mode without exact matches
  $q  = "SELECT * FROM {wisski_vocab_ctrl_index} WHERE ";
  $q .= "value != '%s' AND (value LIKE '%s %%' OR value LIKE '%s-%%')";
  $res = db_query($q, $token[0], $token[0], $token[0], $token[0], $token[0]);

  while ($row = db_fetch_object($res)) {

    $label = $row->value;
    $pos = stripos($label, $token[0]);
    if ($pos === FALSE) continue; // $pos can be false if search term contains diacritica
    $prefix = substr($label, 0, $pos);
    $pos += strlen($token[0]);
    $suffix = substr($label, $pos);
    if ($suffix === FALSE) $suffix = '';

    $voc = wisski_vocab_ctrl_get($row->vid);

    // $prefix == '' && $suffix == '' can never hold

    $does_match = TRUE;
    $token_count = 1;
    $range = array($token[1], $token[2]);

    // test if all leading tokens in label match 
    if ($prefix != '') {
      if (!preg_match('/\\s$/', $prefix)) {
        $does_match = FALSE;
      } else {
        $prefix_tokens = wisski_textproc_tokenize($prefix);
        $token_count += count($prefix_tokens);
        array_reverse($prefix_tokens);
        foreach ($prefix_tokens as $p => $t) {
          $tp = $token_pos - $p - 1;
          if ($tp < 0 || strcasecmp($tokens[$tp][0], $t[0]) != 0) {
            $does_match = FALSE;
            $range = array($token[1], $token[2]);
            break;
          } else {
            $range[0] = $tokens[$tp][1];
          }
        }
      }
    }

    // test if all trailing tokens in label match
    if ($does_match && $suffix != '') {
      if (!preg_match('/^\\s/', $suffix)) {
        $does_match = FALSE;
      } else {
        $suffix_tokens = wisski_textproc_tokenize($suffix);
        $token_count += count($suffix_tokens);
        foreach ($suffix_tokens as $p => $t) {
          $tp = $token_pos + $p + 1;
          if ($tp >= count($tokens) || strcasecmp($tokens[$tp][0], $t[0]) != 0) {
            $does_match = FALSE;
            $range = array($token[1], $token[2]);
            break;
          } else {
            $range[1] = $tokens[$tp][2];
          }
        }
      }
    }

    // we are going to remember the annotation either way
    $anno = array(
        'voc' => $voc->vid,
        'uri' => $row->uri,
        'class' => (string) $voc->group_id,
        'label' => $label,
        'lang' => $row->lang,
        'range' => $range,
        'rank' => $voc->priority,
        );

    // if all tokens matched, we have an overlapping anno, otherwise just a strong guess
    if ($does_match) {

      // do this only for real matches, otherwise it would be too much to compute
      // calculate coordinate offset
      // get the distance to each reference coordinate pair
      // the distance to the nearest pair is taken for offset calculation
      // coordinates must be given in degrees to function
      $contraint_offset = _wisski_textproc_analyse_coord_offset($row, $voc, $settings);

      $anno['rank'] = $anno['rank'] + $rank_factor_length_contains * $token_count + $rank_offset_contains + $constraint_offset;
      $overlap[$voc->priority][] = $anno;
    } else {
      $anno['rank'] = $anno['rank'] + $rank_factor_length_guess * $token_count + $rank_offset_guess;
      $guess[$voc->priority][] = $anno;
    }

  }

  $proto_annos = array_merge_recursive($exact, $overlap);
  if (!$proto_annos) $proto_annos = $guess;

  krsort($proto_annos);
  $tmp =  array_reduce($proto_annos, function($a, $b) { return array_merge($a, $b); }, array());

  return $tmp;
}


/** Helper function for wisski_textproc_analyse_vocab
* Calculate rank factor for a place annotation according to its coordinates.
*
* @author Martin Scholz, Florian Wittmann
*
*/
function _wisski_textproc_analyse_coord_offset($row, $voc, $settings) {

  // calculate coordinate offset
  // get the distance to each reference coordinate pair
  // the distance to the nearest pair is taken for offset calculation
  // coordinates must be given in degrees to function
  $rank_offset_coords_lat = isset($settings['rank_offset_coords_lat']) ? $settings['rank_offset_coords_lat'] : -1.5;
  $rank_offset_coords_long = isset($settings['rank_offset_coords_long']) ? $settings['rank_offset_coords_long'] : -1.5;
  $coords = $settings['place_coords'];
  $place_classes = $settings['place_classes'];

  $coord_offset = 0;
  if ($coords && in_array($voc->group_id, $place_classes)) {
    $coord_offset = $rank_offset_coords_lat + $rank_offset_coords_long;
    $c = wisski_vocab_ctrl_lookup(array('latitude', 'longitude'), $voc, $row->uri, NULL);
    if ($c) {
      $c = array('lat' => $c[$row->uri]['latitude'][0]['value'],'long' => $c[$row->uri]['longitude'][0]['value']);
      foreach ($coords as $coord) {
        $o = $rank_offset_coords_lat + $rank_offset_coords_long
          - (180 - abs(abs($coord['lat'] - $c['lat']) % 180 - floor(abs(abs($coord['lat'] - $c['lat']) / 180)))) * $rank_offset_coords_lat / 180
          - (180 - abs(abs($coord['long'] - $c['long']) % 180 - floor(abs(abs($coord['long'] - $c['long']) / 180)))) * $rank_offset_coords_long / 180;
        if ($coord_offset < $o) $coord_offset = $o;
      }
    }
  }

  return $coord_offset;
}


/** Helper function for wisski_textproc_analyse_vocab
* Calculate rank factor for an annotation according to lemma info.
* If a token has a lemma and a certain pos, this affects the propability that
* it may be a certain type of annotation
*
* @author Martin Scholz, Florian Wittmann
*
*/
function _wisski_textproc_analyse_lemma_offset($token, $voc, $settings) {

  $rank_offset_has_lemma = isset($settings['rank_offset_has_lemma']) ? $settings['rank_offset_has_lemma'] : array('62' => -6.5);
  $lemma_offset = 0;
  // check for lemma
  if (isset($token['lemma']) && $token['lemma'] != '') {
    if (isset($rank_offset_has_lemma[$voc->group_id])) {
      $lemma_offset += $rank_offset_has_lemma[$voc->group_id];
    }
  }
  return $lemma_offset;
}

/** Helper function for wisski_textproc_analyse_vocab
* Calculate rank factor for an annotation according to pos info.
* If a token has a lemma and a certain pos, this affects the propability that
* it may be a certain type of annotation
*
* @author Martin Scholz, Florian Wittmann
*
*/
function _wisski_textproc_analyse_pos_offset($token, $voc, $settings) {

  $rank_offset_has_pos = isset($settings['rank_offset_has_pos']) ? $settings['rank_offset_has_pos'] : array('62' => array('NE' => 0,'NN' => 0, '' => -6));
  $pos_offset = 0;
  // check for pos
  if (isset($token['pos']) && $token['pos'] != '') {
    if (isset($rank_offset_has_pos[$voc->group_id])) {
      if (isset($rank_offset_has_pos[$voc->group_id][$token['pos']])) {
        $pos_offset += $rank_offset_has_pos[$voc->group_id][$token['pos']];
      } else {
        $pos_offset += $rank_offset_has_pos[$voc->group_id][''];
      }
    }
  }
  return $pos_offset;
}



/** Annotate text ranges that follow a certain token pattern
* This is the analyse operation for analysis component type vocab
* 
* First marks all tokens according to a list of words, then
* executes a regex on that annotations. Tokens that are in a match will be
* annotated
*
* @author Martin Scholz
*
*/
function _wisski_textproc_analyse_automaton($profile, $lang, $text, $tokens, $annos, $settings) {

  $annos = array();
  $patterns = $settings['patterns'];

  foreach ($patterns as $k => $pattern) {
    $regex = '';
    foreach ($pattern['states'] as $p) {
      $regex .= $p['type'][0];
      $regex .= $p['occurence'];
    }
    $patterns[$k]['regex'] = $regex;
  }

  // go thru all tokens and annotate with possible class
  $tokens_len = count($tokens);
  $findings = ($tokens_len) ? array_fill(0, $tokens_len, NULL) : array();

  for ($token_pos = 0; $token_pos < $tokens_len; $token_pos++) {
    $token = $tokens[$token_pos];

    // for each token get the possible name parts
    // first, get all parts where the language matches
    $finding = array();
    $rows = db_query("SELECT * FROM {%s} WHERE name = '%s' AND lang = '%s' ORDER BY rank", $settings['db_name'], $token[0], $lang);
    while ($row = db_fetch_array($rows)) {
      if ($token[0] != $row['name']) continue;  // for case sensitivity
      $finding[] = $row;
    }
    
    // second, get all parts where language does not match
    $rows = db_query("SELECT * FROM {%s} WHERE name = '%s' AND lang != '%s' ORDER BY rank", $settings['db_name'], $token[0], $lang);
    while ($row = db_fetch_array($rows)) {
      if ($token[0] != $row['name']) continue;  // for case sensitivity
      $finding[] = $row;
    }

    foreach ($finding as $f) {
      // annotate the right token with the class
      // it doesnt matter if we get out of range, it will be ignored
      if ($findings[$token_pos + $f['offset']] === NULL) $findings[$token_pos + $f['offset']] = array();
      $findings[$token_pos + $f['offset']][] = $f;
    }

  }

  // go through all findings
  $start = 0;
  $end = 0;

  while ($end < $tokens_len) {

    // if we don't find anything, go ahead
    if ($findings[$end] == NULL) {
      $end++;
      $start = $end;
      continue;
    }

    // test each token substring with each pattern
    $find_patt = array('' => array());
    for ($offset = $end; $offset >= $start; $offset--) {

      $anno = array('rank' => 0);

      // construct finding patterns from 
      $new_find_patt = array();
      foreach ($findings[$offset] as $f) {
        foreach ($find_patt as $fp => $info) {
          array_unshift($info, $f);
          $new_find_patt[$f['type'][0] . $fp] = $info;
        }
      }
      $find_patt = $new_find_patt;

      foreach ($patterns as $pattern) {
        $p = $pattern['regex'];
        foreach ($find_patt as $fp => $info) {

          if (preg_match("/^$p$/u", $fp)) {
            $rank = 0;
            foreach ($info as $i) $rank += $i['rank'];
            $rank *= ($end - $offset + 1) * $pattern['rank'];
            if ($rank > $anno['rank']) {
              $a_start = $tokens[$offset][1];
              $a_end = $tokens[$end][2];  // $token_pos is last finding pos + 1!
              $anno = array(
                  'class' => $settings['class'],
                  'rank' => $rank,
                  'range' => array($a_start, $a_end),
                  );
            }
          }
        }
      }

      if ($anno['rank'] > 0) {
        if (isset($settings['factor'])) $anno['rank'] *= $settings['factor'];
        $annos[] = $anno;
      }
    }

    // reposition the end pointer
    // start stays the same, as we might build bigger terms
    $end++;

  }

  return $annos;

}


/** Find annotations that have a certain date syntax
* This is the analyse operation for analysis component type vocab
* 
*
* @author Martin Scholz
*
*/
function _wisski_textproc_analyse_datetime($profile, $lang, $text, $tokens, $annos, $settings) {

  $class = $settings['class'];

  $formats = array(
      DateTime::ISO8601,
      'Y',
      'd#m#y',
      'j#n#y',
      'm#y',
      'd#m#Y',
      'j#n#Y',
      'n#Y',
      'm#Y',
      'M Y',
      'd M Y',
      'd. M Y',
      'Ymd',

      );

  $annos = array();
  $tokens_len = count($tokens);
  for ($s = 0; $s < $tokens_len; $s++) {
    for ($e = $s; $e < $tokens_len; $e++) {
      $str = mb_substr($text, $tokens[$s][1], $tokens[$e][2] - $tokens[$s][1]);
      if (mb_strlen($str) < 3) continue;  // year with only 2 ciphers are ruled out. too many false positives
      
      // use the php date parsing function
      foreach($formats as $f) {
        $tmp = date_parse_from_format($f, $str);
        if (!$tmp['errors']) {
          $annos[] = array(
              'rank' => ($e - $s + 1) * 2,
              'class' => $class,
              'range' => array($tokens[$s][1], $tokens[$e][2]),
              );
        }
      }
      
      // some selfdefined regexes
      if (preg_match('/^(([012]?\d|30|31)\.?)?\s*(Jan(\.|uar)|Feb(\.|ruar)|Mär(\.|z)|Apr(\.|il)|Mai|Jun[\.i]|Jul[\.i]|Aug(\.|ust)|Sep(\.|tember)|Okt(\.|ober)|Nov(\.|ember)|Dez(\.|ember))\s+\d+$/u', $str)) {
        $annos[] = array(
            'rank' => ($e - $s + 1) * 2,
            'class' => $class,
            'range' => array($tokens[$s][1], $tokens[$e][2]),
            );
      }

      if (preg_match('/^(([012]?\d|30|31)(\.|th|rd|st|nd)?\s+)?(Jan(\.|uary)|Feb(\.|ruary)|Mar(\.|ch)|Apr(\.|il)|May|Jun[\.e]|Jul[\.y]|Aug(\.|ust)|Sep(\.|tember)|Oct(\.|ober)|Nov(\.|ember)|Dec(\.|ember))(,?\s+\d+)?$/u', $str)) {
        $annos[] = array(
            'rank' => ($e - $s + 1) * 2,
            'class' => $class,
            'range' => array($tokens[$s][1], $tokens[$e][2]),
            );
      }

      if (preg_match('/^(Jan(\.|uary)|Feb(\.|ruary)|Mar(\.|ch)|Apr(\.|il)|May|Jun[\.e]|Jul[\.y]|Aug(\.|ust)|Sep(\.|tember)|Oct(\.|ober)|Nov(\.|ember)|Dec(\.|ember))(\s+the)?\s+(([012]?\d|30|31)(\.|th|rd|st|nd)?)?(,?\s+\d+)?$/u', $str)) {
        $annos[] = array(
            'rank' => ($e - $s + 1) * 2,
            'class' => $class,
            'range' => array($tokens[$s][1], $tokens[$e][2]),
            );
      }


    }
  }

  return $annos;

}



/** Find annotations that match a regex
* This is the analyse operation for analysis component type wisski_textproc_regexp
* 
*
* @author Martin Scholz
*
*/
function _wisski_textproc_analyse_regexp($profile, $lang, $text, $tokens, $annos, $settings) {

  if (!isset($settings['pattern']) || !isset($settings['class'])) return array();
  $pattern = $settings['pattern'];
  $class = $settings['class'];
  $rank_offset = isset($settings['rank_offset']) ? $settings['rank_offset'] : 1;
  $rank_length_factor = isset($settings['rank_length_factor']) ? $settings['rank_length_factor'] : 0.1;

  $annos = array();

  if (preg_match_all("/$pattern/u", $text, $matches, PREG_PATTERN_ORDER | PREG_OFFSET_CAPTURE)) {

    foreach ($matches[0] as $match) {

      $start = $match[1];
      $len = mb_strlen($match[0]);
      $end = $start + $len;

      $anno = array(
          'class' => $class,
          'range' => array($start, $end),
          'rank' => $rank_offset + ($len * $rank_length_factor),
          );
      $annos[] = $anno;

    }

  }

  return $annos;

}


/** Set relations betweeen annotations
* This function makes use of a db table in which all candidate words for events
* that link annotations are stored.
*
* A word is mapped to one or more ontology classes which in turn map to one or
* more paths/subgroups. From the subgroups, the annotation classes that can
* potentially be linked through that word are calculated.
*
* @author Martin Scholz
*
*/
function wisski_textproc_link_events($profile, $lang, $text, $tokens, $annos, $settings) {

  $ttoa = _wisski_textproc_tokens_to_annos($tokens, $annos);

  $ontology = wisski_editor_build_ontology();
  $paths = array_keys($ontology['properties']);
  
  $events_to_paths = array();
  $result = db_query('SELECT DISTINCT event FROM {%s}', $settings['db_name']);
  while ($r = db_fetch_array($result)) {
    $event = wisski_store_getObj()->wisski_ARCAdapter_delNamespace($r['event']);
    $events_to_paths[$event] = _wisski_textproc_relate_paths_to_event($paths, $event);
  }

  for ($i = 0; $i < count($tokens); $i++) {
    if (isset($ttoa[$i])) continue; // dont treat tokens that are already annotated as events
    $token = $tokens[$i];

    $lemma = (isset($token['lemma']) && $token['lemma'] != '') ? strtolower($token['lemma']) : NULL;
    if ($lemma) {
      $query = "SELECT event, pos FROM {%s} WHERE `lang` = '%s' AND (`name` = '%s' OR `name` = '%s')";
    } else {
      $query = "SELECT event, pos FROM {%s} WHERE `lang` = '%s' AND `name` = '%s'";
    }
    $result = db_query($query, $settings['db_name'], $lang, strtolower($token[0]), $lemma);
    
    // collect all events in this array to sort out duplicates
    $events = array();
    while ($r = db_fetch_array($result)) {
      // check token part-of-speech against list of pos mappings from db entry
      // only if pos info for both is present and does not match, then skip
      if ($r['pos'] && $token['pos'] && isset($settings['pos_mapping'][$r['pos']]) && !in_array($token['pos'], $settings['pos_mapping'][$r['pos']])) continue;
      // this can be further checked
      $events[$r['event']] = 1;
    }

    foreach ($events as $event => $tmp) {
      $event = wisski_store_getObj()->wisski_ARCAdapter_delNamespace($event);
      // there may be no paths
      foreach ($events_to_paths[$event] as $path_id) {
        
        // try to find an instance for the domain
        $domain_class = $ontology['properties'][$path_id]['domain'];
        $domain = NULL;
        
        $j = $i - 1;
        for (; $j >= 0 && !preg_match('/^[.!?]+$/', $tokens[$j][0]); $j--) {  // stop at sentence boundaries, where a boundary is '.', '!', or '?'
          if (!isset($ttoa[$j])) continue; // no anno at this token
          $anno = $annos[$ttoa[$j]];
          if ($anno['class'] == $domain_class) {
            $domain = $ttoa[$j];
            break;
          }
        }
        if ($domain === NULL) { // we found no appropriate domain instance, so search in other direction
          $j = $i + 1;
          for (; $j < count($tokens) && !preg_match('/^[.!?]+$/', $tokens[$j][0]); $j++) {
            if (!isset($ttoa[$j])) continue; // no anno at this token
            $anno = $annos[$ttoa[$j]];
            if ($anno['class'] == $domain_class) {
              $domain = $ttoa[$j];
              break;
            }
          }
          if ($domain === NULL) continue;  // we found no appropriate domain instance
        }
        
        // try to find an instance for the range
        $range_classes = $ontology['properties'][$path_id]['range'];
        $range = NULL;
        
        $j = $i + 1;
        for (; $j < count($tokens) && !preg_match('/^[.!?]+$/', $tokens[$j][0]); $j++) {
          if (!isset($ttoa[$j])) continue; // no anno at this token
          if ($ttoa[$j] == $domain) continue; // don't allow range and domain to be the same instance!
          $anno = $annos[$ttoa[$j]];
          if (in_array($anno['class'], $range_classes)) {
            $range = $ttoa[$j];
            break;
          }
        }
        if ($range === NULL) {  // we found no appropriate domain instance, so search in other direction
          $j = $i - 1;
          for (; $j >= 0 && !preg_match('/^[.!?]+$/', $tokens[$j][0]); $j--) {
            if (!isset($ttoa[$j])) continue; // no anno at this token
            if ($ttoa[$j] == $domain) continue; // don't allow range and domain to be the same instance!
            $anno = $annos[$ttoa[$j]];
            if (in_array($anno['class'], $range_classes)) {
              $range = $ttoa[$j];
              break;
            }
          }
          if ($range === NULL) continue;  // we found no appropriate domain instance
        }

        if (!isset($annos[$domain]['rel'])) $annos[$domain]['rel'] = array($path_id => array());
        elseif (!isset($annos[$domain]['rel'][$path_id])) $annos[$domain]['rel'][$path_id] = array();
        $annos[$domain]['rel'][$path_id][] = $annos[$range]['uri'];
          
      } 

    }    

  }
  
  return $annos;

}


/** Helper function for wisski_textproc_link_events()
*
* @author Martin Scholz
*
*/
function _wisski_textproc_relate_paths_to_event($paths, $event) {
  
  $relpaths = array();
  
  // go through each path and check whether the event class occurs in path
  // if occurs, add to return set
  foreach ($paths as $path_id) {
    $path_data = wisski_pathbuilder_getPathData($path_id);
    $path_array = unserialize($path_data['path_array']);
    for ($i = 1; isset($path_array['x' . ($i + 1)]); $i++) {
      if ($path_array["x$i"] == $event) {
        $relpaths[] = $path_id;
        break;
      }
    }
  }
  
  return $relpaths;
  
}


/** Helper function for wisski_textproc_link_events()
* @return a mapping of tokens to the annotation that contains them
* the mapping will only include the indices of annotations and tokens in the
* passed arrays.
*
* @author Martin Scholz
*
*/
function _wisski_textproc_tokens_to_annos($tokens, $annos) {

  $ttoa = array();
  for ($a = 0; $a < count($annos); $a++) {
    $i = 0;
    for (; $i < count($tokens); $i++) {
      if ($annos[$a]['range'][0] <= $tokens[$i][1]) {
        break;
      }
    }
    for (; $i < count($tokens); $i++) {
      if ($annos[$a]['range'][1] >= $tokens[$i][2]) {
        $ttoa[$i] = $a;
      } else {
        break;
      }
    }
  }
  return $ttoa;
}
