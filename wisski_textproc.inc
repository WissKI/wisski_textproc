<?php



/** A tokenizer
* returns a sequence of tokens each being an array of the form
* array(<surface>, <start>, <end>).
* <surface> will be equal to mb_substring($text, $token[1], $token[2])
*
* @author Martin Scholz
*
*/
function wisski_textproc_tokenize($text, $lang = '') {

  $settings = db_query("SELECT settings FROM {wisski_textproc_pipes} WHERE profile = 'tokenizer' AND lang = '%d'", $lang);
  $settings = db_fetch_array($abbr);
  if ($settings != NULL) $settings = $settings['settings'];
  if ($settings == NULL) $settings = array();
  $abbr = isset($settings['abbreviation']) ? $settings['abbreviation'] : array();

  if (!preg_match_all('/[^\\p{Z}\\p{Cc}\\p{Cf}]+/u', $text, $matches, PREG_PATTERN_ORDER|PREG_OFFSET_CAPTURE)) return array();

  $tokens = array();
  foreach ($matches[0] as $m) {

    // offset capture broken with utf8. workaround: 
    $m[1] = mb_strlen(substr($text, 0, $m[1]));

    $m[2] = $m[1] + mb_strlen($m[0]); // end offset of match

    if (isset($abbr[$m[0]]) ||
        preg_match('/^[\\p{L}\\p{N}]+(\\.[\\p{L}\\p{N}]+)*$/u', $m[0]) ||
        preg_match_all('/\\p{P}+/u', $m[0], $punct, PREG_PATTERN_ORDER|PREG_OFFSET_CAPTURE) == 0) {

      // no punctuation marks in match => take it as a whole token
      $tokens[] = $m;

    } else {

      // split match into tokens according to puncts
      $last_offset = 0;
      foreach ($punct[0] as $p) {

        $p[1] = mb_strlen(substr($m[0], 0, $p[1]));

        $token = array(  // token before this punct
            mb_substr($m[0], $last_offset, $p[1] - $last_offset),
            $m[1] + $last_offset,
            $m[1] + $p[1],
            );
        $tokens[] = $token;

        // one token for each char
        $l = mb_strlen($p[0]);
        for ($i = 0; $i < $l; $i++) {
          $token = array(mb_substr($p[0], $i, 1), $m[1] + $p[1] + $i, $m[1] + $p[1] + $i + 1);
          $tokens[] = $token;
        }

        $last_offset = $p[1] + $l;

      }

      if ($m[2] != $m[1] + $last_offset) {
        $token = array(
            mb_substr($m[0], $last_offset, mb_strlen($m[0])),
            $m[1] + $last_offset,
            $m[2],
            );
        $tokens[] = $token;
      }

    }
  }

  foreach ($tokens as $k => $t) {
    if (trim($t[0]) == '') unset($tokens[$k]);
  }
  $tokens = array_values($tokens);

  return $tokens;

}



/** Internal implementation of wisski_textproc_strip_tags()
*
* @author Martin Scholz
*/
function _wisski_textproc_strip_tags($xhtml, $default_lang = '') {

  $ws_replacements = array('br', 'p', '/p', 'div', '/div');

  $xhtml = "<div>$xhtml</div>"; // encapsulate: text may be xml/html snippet (leading/trailing chars or multiple root tags)
  $doc = DOMDocument::loadXML($xhtml, LIBXML_NOERROR);
  if (!$doc) {
    $doc = DOMDocument::loadHTML('<?xml encoding="UTF-8">' . $xhtml);
  }
  if (!$doc) {
    return NULL;
  }

  list($text, $textlen, $char_shifts, $lang_ranges, $annos) = _wisski_textproc_strip_tags_walk($doc->documentElement, $ws_replacements, '', 0, array(), array(), $default_lang, array());

  $lang_ranges = _wisski_textproc_join_lang_ranges($lang_ranges);

  return array('text' => $text, 'lang_ranges' => $lang_ranges, 'char_shifts' => $char_shifts, 'annos' => $annos);

}


/** Helper function for _wisski_textproc_strip_tags()
* that walks the DOM tree collecting the information
*
* @author Martin Scholz
*
*/
function _wisski_textproc_strip_tags_walk($element, $replacements, $text, $textlen, $char_shifts, $langs, $cur_lang, $annos) {

  if ($element->hasAttribute('lang')) $cur_lang = $element->getAttribute('lang');
  if ($element->hasAttribute('xml:lang')) $cur_lang = $element->getAttribute('xml:lang');

  if (in_array(strtolower($element->tagName), $replacements)) {
    $text .= ' ';
    $langs[$cur_lang][] = array($textlen, $textlen + 1);
    $textlen += 1;
    $char_shifts[] = $textlen;
  }

  $anno = NULL;
  if ($element->hasAttribute('class') && strpos($element->getAttribute('class'), 'wisski_anno') !== FALSE) {
    $anno = array('approved' => FALSE, 'range' => array($textlen));
    foreach(explode(' ', $element->getAttribute('class')) as $class) {
      if (substr($class, 0, 19) == 'wisski_anno_deleted') $anno['deleted'] = TRUE;
      if (substr($class, 0, 18) == 'wisski_anno_class_') $anno['class'] = urldecode(substr($class, 18));
      if (substr($class, 0, 16) == 'wisski_anno_uri_') $anno['uri'] = urldecode(substr($class, 16));
      if (substr($class, 0, 18) == 'wisski_anno_vocab_') $anno['voc'] = urldecode(substr($class, 18));
      if (substr($class, 0, 17) == 'wisski_anno_rank_') $anno['rank'] = urldecode(substr($class, 17));
      if (substr($class, 0, 20) == 'wisski_anno_new') $anno['new'] = TRUE;
      if (substr($class, 0, 20) == 'wisski_anno_proposed') $anno['approved'] = FALSE;
      if (substr($class, 0, 20) == 'wisski_anno_approved') $anno['approved'] = TRUE;
      if (substr($class, 0, 16) == 'wisski_anno_rel_') {
        $rel = explode(':', substr($class, 16));
        $anno['rel'][urldecode($rel[0])][] = urldecode($rel[1]);
      }
      if (substr($class, 0, 16) == 'wisski_anno_rev_') {
        $rel = explode(':', substr($class, 16));
        $anno['rev'][urldecode($rel[0])][] = urldecode($rel[1]);
      }
    }
  }

  $child = $element->firstChild;
  while ($child) {

    switch ($child->nodeType) {
      case XML_TEXT_NODE:
      case XML_CDATA_SECTION_NODE:

        $l = $textlen;
        $text .= _wisski_textproc_normalize_space($child->textContent);
        $textlen += mb_strlen($child->textContent);
        $langs[$cur_lang][] = array($l, $textlen);
        break;

      case XML_ELEMENT_NODE:

        list($text, $textlen, $char_shifts, $langs, $annos) = _wisski_textproc_strip_tags_walk($child, $replacements, $text, $textlen, $char_shifts, $langs, $cur_lang, $annos);
        break;

    }

    $child = $child->nextSibling;

  }

  if ($anno != NULL) {
    $anno['range'][1] = $textlen;
    $annos[] = $anno;
  }

  if (in_array('/' . strtolower($element->tagName), $replacements)) {
    $text .= ' ';
    $langs[$cur_lang][] = array($textlen, $textlen + 1);
    $textlen += 1;
    $char_shifts[] = $textlen;
  }

  return array($text, $textlen, $char_shifts, $langs, $annos);

}


/**
* @author Martin Scholz
*/
function _wisski_textproc_normalize_space($text) {

  $spaces = mb_convert_encoding("\f\n\r\t&nbsp;", "UTF-8", "HTML-ENTITIES");
  return mb_ereg_replace("/[$spaces]/", " ", $text);

}


/**
* @author Martin Scholz
*/
function _wisski_textproc_join_lang_ranges($lang_ranges) {

  foreach ($lang_ranges as $lang => $ranges) {
    // ranges should be sorted! usort($ranges, function($a, $b) { return $a[0] - $b[0]; });
    for ($i = 0; $i < count($ranges) - 1; $i++) {
      while(isset($ranges[$i + 1]) && $ranges[$i][1] == $ranges[$i + 1][0]) {
        $range = array_splice($ranges, $i, 1);
        $ranges[$i][0] = $range[0][0];
      }
    }
    $lang_ranges[$lang] = $ranges;
  }

  return $lang_ranges;

}


/** Take a HTML fragment and generate triples from its annotations.
* Returns all triples generated from the text.
*
* @author Martin Scholz
*
*/
function _wisski_textproc_triplify_xhtml($dom_fragment, $doc_uri, $params = array()) {

  // Parse html into annotations
  $text_struct = _wisski_textproc_strip_tags($dom_fragment);

  // get path definitions for refers to, subject, etc.
  $paths = variable_get('wisski_textproc_paths', array());
  
  // in case the text is about an unknown new subject
  // the group of the subject has been stored.
  // this will later be used to determine the subject entity annotation.
  $subject_group_id = variable_get('wisski_textproc_text_group_id_' . $doc_uri, NULL);
  variable_del('wisski_textproc_text_group_id_' . $doc_uri);

  // if the html doc node has no uri, create one (ie. when new text is created)
  if (!$doc_uri) {
    $doc_uri = wisski_instance_prefix() . wisski_get_uuid(4) . '_text';
  }

  // here, we store all created triples
  $triples = array();

  include_once(drupal_get_path('module', 'wisski_pathbuilder') . '/wisski_pathbuilder.inc');

  // make the document an instance of doc class
  // take first class of path array
  if ($paths['doc_class']) {
    $tmp = wisski_pathbuilder_getPathData($paths['doc_class']);
    if ($tmp['is_group']) {
      $tmp = _wisski_pathbuilder_calculate_group_samepart($paths['doc_class']);
    } else {
      $tmp = unserialize($tmp['path_array']);
    }
    $class = $tmp['x0'];
    $triples[] = array(
        's' => $doc_uri,
        's_type' => 'uri',
        'p' => 'http://www.w3.org/1999/02/22-rdf-syntax-ns#type',
        'o' => $class,
        'o_type' => 'uri',
        );
  }

  // generate creation event // stamp with creator notice
  if ($paths['creator']) {
    $creator = isset($params['creator_name']) ?  $params['creator_name'] :  user_load($user)->name; 
    $tr = _wisski_pathbuilder_generate_triples_of_path(NULL, $creator, $paths['creator'], $doc_uri);
    $triples = array_merge($triples, $tr);
  }

  $annos = array();   // this array will contain only valid annotations

  foreach ($text_struct['annos'] as $anno) {

    if (isset($anno['deleted'])) continue;  // we don't bother about deleted spans

    $voc = isset($anno['voc']) ? $anno['voc'] : NULL;

    // check, if there is already an instance with that uri in the local store
    // if not, we need to know the vocab in order to import it
    if (!wisski_vocab_ctrl_is_imported($voc, $anno['uri'])) {

      // determine vocab and group
      $voc = NULL;
      if (isset($anno['voc'])) {
        $voc = wisski_vocab_ctrl_get($anno['voc']);
        if ($voc == NULL) continue; // invalid vocabulary
        $class = $voc->group_id;
      } else {
        // if voc is not set, it is a local voc
        $class = $anno['class'];
        foreach (wisski_vocab_ctrl_get() as $v) {
          if ($v->group_id == $class && wisski_accesspoint_get($v->aid)->type == 'local') {
            $voc = $v;
            break;
          }
        }
      }

      if ($voc) {        
        if (wisski_accesspoint_get($voc->aid)->type == 'local') {
          $label = mb_substr($text_struct['text'], $anno['range'][0], $anno['range'][1] - $anno['range'][0]);
          $tr = _wisski_pathbuilder_generate_triples_of_path(NULL, $label, $voc->fields['label']['cis_pid'], $anno['uri']);
          $path_data = wisski_pathbuilder_getPathData($voc->fields['label']['cis_pid']);
          $path_array = unserialize($path_data['path_array']);
          $tr[] = array(
              's' => $anno['uri'],
              's_type' => 'uri',
              'p' => 'http://www.w3.org/1999/02/22-rdf-syntax-ns#type',
              'o' => $path_array['x0'],
              'o_type' => 'uri',
              );
          $triples = array_merge($triples, $tr);
        } else {
          $tr = wisski_vocab_ctrl_get_triples($voc->vid, $anno['uri']);
          $triples = array_merge($triples, $tr);
        }
      } else {
        // if there is no voc, we don't know the path where to store the label
        // so just make an <instance typeof class> triple
        $path_array = _wisski_pathbuilder_calculate_group_samepart($anno['class']);
        $t = array(
            's' => $anno['uri'],
            's_type' => 'uri',
            'p' => 'http://www.w3.org/1999/02/22-rdf-syntax-ns#type',
            'o' => $path_array['x0'],
            'o_type' => 'uri',
            );
        $triples[] = $t;
      }

    }

    if ($subject_group_id && $anno['class'] == $subject_group_id) {
      wisski_textproc_set_subject($doc_uri, $anno['uri']);
      $subject_group_id = NULL;
    }


    if ($params['refers_to']) {
      $path_data = wisski_pathbuilder_getPathData($params['refers_to']);
      if ($path_data['is_group']) {
        $path_array = _wisski_pathbuilder_calculate_group_samepart($params['refers_to']);
      } else {
        $path_array = unserialize($path_data['path_array']);
      }
      $i = 2;
      $last_s = $doc_uri;
      $proto_uri = wisski_instance_prefix() . md5($doc_uri) . md5($anno['uri']) . "_";
      while (isset($path_array["x$i"])) {
        $next_s = "$proto_uri$i";
        $triples[] = array(
            's' => $next_s,
            's_type' => 'uri',
            'p' => 'http://www.w3.org/1999/02/22-rdf-syntax-ns#type',
            'o' => $path_array['x' . ($i-1)],
            'o_type' => 'uri',
            );
        $triples[] = array(
            's' => $last_s,
            's_type' => 'uri',
            'p' => $path_array["y" . ($i-2)],
            'o' => $next_s,
            'o_type' => 'uri',
            );
        $last_s = $next_s;
        $i++;
      }

      $triples[] = array(
          's' => $last_s,
          's_type' => 'uri',
          'p' => $path_array["y" . ($i-2)],
          'o' => $anno['uri'],
          'o_type' => 'uri',
          );

    }

    $annos[$anno['uri']][] = $anno;

  }

  // triples may have been created twice...
  $triples = array_unique($triples, SORT_REGULAR);


  // make triples from relations
  foreach ($annos as $anno_a) {
    foreach ($anno_a as $anno) {
      if (!$anno['rel']) continue;

      $class = NULL;
      if (isset($anno['class'])) {
        $class = $anno['class'];
      } else {
        $voc = wisski_vocab_ctrl_get($anno['voc']);
        $class = $voc->group_id;
      }

      // group paths into subgroups
      // determine which subgroups have to be built
      // devide into terminal paths (where the values go) and intermediate paths (ie. subgroups)
      $terminal_rels = array();
      $interm_rels = array();
      foreach ($anno['rel'] as $rel => $targets) {
        $rel_path_data = wisski_pathbuilder_getPathData($rel);
        $terminal_rels[$rel] = $targets;
        while ($rel_path_data['group_id'] != $class) {
          $rel_path_data = wisski_pathbuilder_getPathData($rel_path_data['group_id']);
          if (!in_array($rel_path_data['id'], $interm_rels)) $interm_rels[] = $rel_path_data['id'];
        }
      }
      array_unique($interm_rels);

      // make triples for subgroups and store uris of last instance in path as starting instance of subpaths/groups
      $group_sources = array($class => $anno['uri']);
      foreach ($interm_rels as $rel) {
        $triples = array_merge($triples, _wisski_textproc_triplify_relation($group_sources, $rel));
      }

      foreach ($terminal_rels as $rel => $targets) {
        foreach ($targets as $target) {

          if (!isset($annos[$target])) continue;  // there is no annotation with the target uri

          if (isset($annos[$target][0]['class'])) {
            $class = $annos[$target][0]['class'];
          } elseif (isset($annos[$target][0]['voc'])) {
            $voc = wisski_vocab_ctrl_get($annos[$target][0]['voc']);
            if (!$voc) continue;
            $class = $voc->group_id;
          } else {
            continue; // cannot determine neither class nor voc of target anno
          }

          $triples = array_merge($triples, _wisski_textproc_triplify_relation($group_sources, $rel, $target, $class));
        }
      }
    }
  }

  $triples = array_unique($triples, SORT_REGULAR);
  return $triples;

}


/** Helper function for _wisski_textproc_triplify_xhtml
* Make triples for a relation between two annotations
* This also generates instances in the paths
*
* @author Martin Scholz
*
*/
function _wisski_textproc_triplify_relation(&$group_sources, $rel, $target_uri = NULL, $target_class = NULL) {

  include_once(drupal_get_path('module', 'wisski_pathbuilder') . '/wisski_pathbuilder.inc');

  $path_data = wisski_pathbuilder_getPathData($rel);
  $source = $group_sources[$path_data['group_id']];

  if ($target_class) {
    $tmp1 = wisski_pathbuilder_getPathData($target_class);
    $tmp2 = _wisski_pathbuilder_calculate_group_samepart($target_class);
    if (!$tmp1['group_id']) {  // top group
      $target_class = $tmp2['x0']; // this should be needless, as samapart of top group should be only first concept
    } else {
      $target_class = $tmp2['x' . (floor(count($tmp2) / 2) + 1)];
    }
  }

  $triples = array();

  global $base_root, $base_path;
  $proto_uri = $base_root . $base_path . 'content/' . md5("$source $rel") . md5("$target $target_class") . "_";

  if ($path_data['is_group']) {
    $path_array = _wisski_pathbuilder_calculate_group_samepart($rel);
  } else {
    $path_array = unserialize($path_data['path_array']);
  }

  $group_path_array = _wisski_pathbuilder_calculate_group_samepart($path_data['group_id']);

  $start_step = floor(count($group_path_array) / 2);

  $i = $start_step + 1;
  $last_s = $source;

  while (isset($path_array["x$i"])) {

    if ($path_array["x$i"] == $target_class) {

      $triples[] = array(
          's' => $last_s,
          's_type' => 'uri',
          'p' => $path_array['y' . ($i-1)],
          'o' => $target_uri,
          'o_type' => 'uri',
          );
      break;

    } else {

      $next_s = "$proto_uri$i";

      $triples[] = array(
          's' => $last_s,
          's_type' => 'uri',
          'p' => $path_array['y' . ($i-1)],
          'o' => $next_s,
          'o_type' => 'uri',
          );
      $triples[] = array(
          's' => $next_s,
          's_type' => 'uri',
          'p' => 'http://www.w3.org/1999/02/22-rdf-syntax-ns#type',
          'o' => $path_array["x$i"],
          'o_type' => 'uri',
          );

      $last_s = $next_s;
      $i++;

    }

    if ($target_uri == NULL) $group_sources[$rel] = $last_s;

  }

  return $triples;

}


/** @see wisski_textproc_get()
*
* @author Martin Scholz
*/
function _wisski_textproc_get($aid, $col, $reload) {
  static $aps = array();
  static $all = FALSE;

  if ($reload) {
    $aps = array();
    $all = FALSE;
  }

  if (!$all && (!$aid || !isset($aps[$aid]))) {
    $rows = db_query('SELECT * FROM {wisski_textproc_pipes}' . ((!$aid) ? ' ORDER BY weight ASC' : ' WHERE aid = %d'), $aid);
    while (TRUE) {
      $ap = db_fetch_object($rows);
      if (!$ap) break;
      $ap->settings = unserialize($ap->settings);
      $aps[$ap->aid] = $ap;
    }
    if (!$aid) $all = TRUE;
  }

  if (!$aid) {
    return $aps;
  } elseif (!isset($aps[$aid])) {
    return NULL;
  } elseif (!$col) {
    return $aps[$aid];
  } elseif (!isset($aps[$aid]->$col)) {
    return NULL;
  } else {
    return $aps[$aid]->$col;
  }

}

/** @see wisski_textproc_save()
*
* @author Martin Scholz
*/
function _wisski_textproc_save(&$ap) {
  $ap->settings = serialize($ap->settings);

  if ($ap->aid) {
    // update
     db_query("UPDATE {wisski_textproc_pipes} SET name = '".$ap->name."', settings='%s' WHERE aid = '".$ap->aid."'", $ap->settings);
  } else {
    // insert
    db_query("INSERT INTO {wisski_textproc_pipes} (name,profile,lang,weight,type,module,settings) VALUES ('%s','%s','%s',%s,'%s','%s','%s')",
    $ap->name,$ap->profile,$ap->lang,$ap->weight,$ap->type,$ap->module,$ap->settings);
  }

}

/** @see wisski_textproc_get_types()
* @author Martin Scholz
*/
function _wisski_textproc_get_types($type, $col) {
  static $types = array();

  if (!$types) {
    foreach (module_implements('wisski_textproc') as $module) {
      foreach (module_invoke($module, 'wisski_textproc', 'list') as $t => $info) {
        $info['module'] = $module;
        $info['id'] = $t;
        $types[$t] = $info;
      }
    }
  }

  if (!$type) {
    return $types;
  } elseif (!isset($types[$type])) {
    return NULL;
  } elseif (!$col) {
    return $types[$type];
  } else {
    return $types[$type][$col];
  }

}


/**
* @author Martin Scholz
*/
function wisski_textproc_wisski_textproc_wisski_textproc_vocab($op, $ap) {

  switch ($op) {

    case 'operations':

      return array(t('Edit') => 'admin/settings/wisski/textproc/' . $ap->aid . '/edit');

    case 'edit':

      drupal_load('module', 'wisski_pathbuilder');
      include_once(drupal_get_path('module', 'wisski_pathbuilder') . '/wisski_pathbuilder.inc');

      //for pb_group: display every top group of the import PB section
      $pb_groups = array();
      foreach (wisski_pathbuilder_getEnabledGroups(3) as $id) {
        $info = wisski_pathbuilder_getPathData($id);
        if ($info['group_id'] < 1) $pb_groups["$id"] = $info['name'];
      }

      if (!$ap->settings) {
        $ap->settings = array(
            'place_classes' => array(),
            'place_use_approved_coordinates' => '0',
            'place_coords' => array(),
            'rank_offset_coords_lat' => '0',
            'rank_offset_coords_long' => '0',
            'rank_offset_exact' => '0',
            'rank_factor_length_contains' => '0',
            'rank_offset_contains' => '0',
            'rank_factor_length_guess' => '0',
            'rank_offset_guess' => '0',
            'rank_offset_has_lemma' => array(),
            'rank_offset_has_pos' => array(),
            );

        foreach ($pb_groups as $id => $grp) {
          $ap->settings['rank_offset_has_lemma'][$id] = "0";
          $ap->settings['rank_offset_has_pos'][$id] = array();
        }

      }

      $coords_str = '';
      foreach ($ap->settings['place_coords'] as $coords) {
        $coords_str .= $coords['lat'] . ' ' . $coords['long'] . "\n";
      }

      $fieldset = array();
      $items['frame_vocab'] = array(
          '#type' => 'fieldset',
          '#title' => t('vocabulary detection'),
          '#weight' => 2,
          'frame_places' => array(
            '#type' => 'fieldset',
            '#title' => t('place coordinates check'),
            'place_classes' => array(
              '#type' => 'textfield',
              '#title' => t('place classes'),
              '#default_value' =>  join(' ', $ap->settings['place_classes']),
              ),
            'use_coordinates_approved' => array(
              '#type' => 'checkbox',
              '#title' => t('use coordinates of approved annotations'),
              '#default_value' => $ap->settings['place_use_approved_coordinates'],
              ),
            'place_coords' => array(
              '#type' => 'textarea',
              '#title' => t('preferred coords'),
              '#default_value' =>  $coords_str,
              '#description' => t('each line contains separated by space: latitude, longitude'),
              ),

            'place_lat_factor' => array(
              '#type' => 'textfield',
              '#title' => t('Latitude factor'),
              '#default_value' => $ap->settings['rank_offset_coords_lat'],
              ),
            'place_long_factor' => array(
                '#type' => 'textfield',
                '#title' => t('Longitude factor'),
                '#default_value' => $ap->settings['rank_offset_coords_long'],
                ),
            ),

            'rank_offset_exact' => array(
                '#type' => 'textfield',
                '#title' => t('Rank offset exact'),
                '#default_value' => $ap->settings['rank_offset_exact'],
                ),
            'rank_factor_length_contains' => array(
                '#type' => 'textfield',
                '#title' => t('Rank factor length contains'),
                '#default_value' => $ap->settings['rank_factor_length_contains'],
                ),
            'rank_offset_contains' => array(
                '#type' => 'textfield',
                '#title' => t('Rank offset contains'),
                '#default_value' => $ap->settings['rank_offset_contains'],
                ),

            'rank_factor_length_guess' => array(
                '#type' => 'textfield',
                '#title' => t('Rank factor length guess'),
                '#default_value' => $ap->settings['rank_factor_length_guess'],
                ),
            'rank_offset_guess' => array(
                '#type' => 'textfield',
                '#title' => t('Rank offset guess'),
                '#default_value' => $ap->settings['rank_offset_guess'],
                ),

            'has_lemma' => array(
                '#type' => 'fieldset',
                '#title' => t('has lemma'),
                '#collapsible' => TRUE,
                '#collapsed' => TRUE,
                ),

            'has_pos' => array(
                '#type' => 'fieldset',
                '#title' => t('has pos'),
                '#collapsible' => TRUE,
                '#collapsed' => TRUE,
                ),
            );

      foreach ($pb_groups as $id => $grp) {
        $items['frame_vocab']['has_lemma']['has_lemma_'.$id] = array(
            '#type' => 'textfield',
            '#title' => t($grp),
            '#default_value' => $ap->settings['rank_offset_has_lemma'][$id],
            );
      }

      foreach ($pb_groups as $id => $grp) {

        $str = "";
        if (isset($ap->settings['rank_offset_has_pos'][$id]) && !empty($ap->settings['rank_offset_has_pos'][$id])) {
          foreach ($ap->settings['rank_offset_has_pos'][$id] as $key => $val) {
            $tmp = "";
            if ($key == "") {
              $tmp = $val;
            } else {
              $tmp = $key." ".$val;
            }
            if ($str =="") {
              $str =$tmp;
            } else {
              $str = $str."\n".$val;
            }
          }
        }
        $items['frame_vocab']['has_pos']['has_pos_'.$id] = array(
            '#type' => 'textarea',
            '#title' => t($grp),
            '#default_value' => $str,
            );
      }

      return $items;

    case 'validate':
      return;
    case 'submit':
      drupal_load('module', 'wisski_pathbuilder');
      include_once(drupal_get_path('module', 'wisski_pathbuilder') . '/wisski_pathbuilder.inc');

      //for pb_group: display every top group of the import PB section
      $pb_groups = array();
      foreach (wisski_pathbuilder_getEnabledGroups(3) as $id) {
        $info = wisski_pathbuilder_getPathData($id);
        if ($info['group_id'] < 1) $pb_groups["$id"] = $info['name'];
      }

      //update vocab:
      $vocab_settings = array();
      $coords = array();
      foreach (explode("\n", $ap->place_coords) as $cline) {
        $cline = preg_split("/ +/", $cline, -1, PREG_SPLIT_NO_EMPTY);
        if (count($cline) != 2) continue;
        $coords[] = array_combine(array('lat', 'long'), $cline);
      }

      $vocab_settings['place_coords'] = $coords;
      $vocab_settings['rank_offset_coords_lat'] = $ap->place_lat_factor;
      $vocab_settings['rank_offset_coords_long'] = $ap->place_long_factor;
      $vocab_settings['place_use_approved_coordinates'] = $ap->use_coordinates_approved;
      $vocab_settings['place_classes'] = preg_split('/ +/', $ap->place_classes, PREG_SPLIT_NO_EMPTY);
      $vocab_settings['rank_offset_exact'] = $ap->rank_offset_exact;

      $vocab_settings['rank_offset_exact'] = $ap->rank_offset_exact;
      $vocab_settings['rank_offset_contains'] = $ap->rank_offset_contains;
      $vocab_settings['rank_offset_guess'] = $ap->rank_offset_guess;
      $vocab_settings['rank_factor_length_contains'] = $ap->rank_factor_length_contains;
      $vocab_settings['rank_factor_length_guess'] = $ap->rank_factor_length_guess;

      $apArray = (array) $ap;

      $vocab_settings['rank_offset_has_lemma'] = array();
      foreach ($pb_groups as $id => $grp) {
        $vocab_settings['rank_offset_has_lemma'][$id] = $apArray['has_lemma_'.$id];
      }

      $vocab_settings['rank_offset_has_pos'] = array();
      foreach ($pb_groups as $id => $grp) {
        $has_pos = wisski_textproc_build_has_pos_reverse($apArray['has_pos_'.$id]);
        $vocab_settings['rank_offset_has_pos'][$id] = $has_pos;
      }

      return array($vocab_settings, NULL);

    default:

      return NULL;

  }

}





/**
* @author Martin Scholz
*/
function wisski_textproc_wisski_textproc_wisski_textproc_person($op, $ap) {

  switch ($op) {

    case 'operations':

      return array(t('Edit') => 'admin/settings/wisski/textproc/' . $ap->aid . '/edit');

    case 'edit':

      drupal_load('module', 'wisski_pathbuilder');
      include_once(drupal_get_path('module', 'wisski_pathbuilder') . '/wisski_pathbuilder.inc');

      //for pb_group: display every top group of the import PB section
      $pb_groups = array();
      foreach (wisski_pathbuilder_getEnabledGroups(3) as $id) {
        $info = wisski_pathbuilder_getPathData($id);
        if ($info['group_id'] < 1) $pb_groups["$id"] = $info['name'];
      }

      if (!$ap->settings) {
        $ap->settings = array(
            'class' => '',
            'db_name' => 'wisski_textproc_personnames',
            'patterns' => wisski_textproc_buildrankingsreverse("3 g+s\n2 scg+\n1 g+\n1 s"),
            );
      }

      $patterns = wisski_textproc_buildrankings($ap->settings['patterns']);

      $fieldset = array();
      $fieldset['type_settings_persongroups'] =  array(
          '#type' => 'select',
          '#title' => t('Group'),
          '#multiple' => false ,
          '#options' => $pb_groups,
          '#default_value' => $ap->settings['class'],
          );
      $fieldset['type_settings_dbname'] = array(
          '#type' => 'textfield',
          '#title' => t('Database table name'),
          '#default_value' => $ap->settings['db_name'],
          );
      $fieldset['type_settings_patterns'] = array(
          '#type' => 'textarea',
          '#title' => t('Rankings'),
          '#default_value' => $patterns,
          '#description' => t('Each line contains a pattern (s = surname, g = givenname, a = name addendum, c = comma, + = multiple, ? = optional) preceeded by a factor and a whitespace.'),
          );    

      return $fieldset;

    case 'validate':
      return;

    case 'submit':
      $pattern = wisski_textproc_buildrankingsreverse($ap->type_settings_patterns);

      $submit = array(
          array(
            'class' => $ap->type_settings_persongroups,
            'db_name' => $ap->type_settings_dbname,
            'patterns' => $pattern,
            ),
          NULL
          );
      return $submit;

    default:

      return NULL;

  }

}

/**
* @author Martin Scholz
*/
function wisski_textproc_wisski_textproc_wisski_textproc_regexp($op, $ap) {

  switch ($op) {

    case 'operations':

      return array(t('Edit') => 'admin/settings/wisski/textproc/' . $ap->aid . '/edit');

    case 'edit':

      drupal_load('module', 'wisski_pathbuilder');
      include_once(drupal_get_path('module', 'wisski_pathbuilder') . '/wisski_pathbuilder.inc');

      //for pb_group: display every top group of the import PB section
      $pb_groups = array();
      foreach (wisski_pathbuilder_getEnabledGroups(3) as $id) {
        $info = wisski_pathbuilder_getPathData($id);
        if ($info['group_id'] < 1) $pb_groups["$id"] = $info['name'];
      }

      if (!$ap->settings) {
        $ap->settings = array(
            'class' => '',
            );
      }

      $fieldset = array();
      $fieldset['type_settings_regexp_groups'] = array(
          '#type' => 'select',
          '#title' => t('group regexp'),
          '#multiple' => false,
          '#options' => $pb_groups,
          '#default_value' =>  $ap->settings['class'],
          );
      $fieldset['type_settings_regexp_pattern'] = array(
          '#type' => 'textfield',
          '#title' => t('pattern'),
          '#default_value' => $ap->settings['pattern'],
          );  
      $fieldset['type_settings_regexp_rank_offset'] = array(
          '#type' => 'textfield',
          '#title' => t('rank offset'),
          '#default_value' => $ap->settings['rank_offset'],
          );
      $fieldset['type_settings_regexp_rank_length_factor'] = array(
          '#type' => 'textfield',
          '#title' => t('rank length factor'),
          '#default_value' => $ap->settings['rank_length_factor'],
          );          
      return $fieldset;     

    case 'validate':

      return;

    case 'submit':
      return array(
          array(
            'class' => $ap->type_settings_regexp_groups,
            'pattern' => $ap->type_settings_regexp_pattern,
            'rank_offset' => $ap->type_settings_regexp_rank_offset,
            'rank_length_factor' => $ap->type_settings_regexp_rank_length_factor,
            ),
          NULL
          );

    default:

      return NULL;

  }

}


/**
* @author Martin Scholz
*/
function wisski_textproc_wisski_textproc_wisski_textproc_datetime($op, $ap) {

  switch ($op) {

    case 'operations':

      return array(t('Edit') => 'admin/settings/wisski/textproc/' . $ap->aid . '/edit');

    case 'edit':

      drupal_load('module', 'wisski_pathbuilder');
      include_once(drupal_get_path('module', 'wisski_pathbuilder') . '/wisski_pathbuilder.inc');

      //for pb_group: display every top group of the import PB section
      $pb_groups = array();
      foreach (wisski_pathbuilder_getEnabledGroups(3) as $id) {
        $info = wisski_pathbuilder_getPathData($id);
        if ($info['group_id'] < 1) $pb_groups["$id"] = $info['name'];
      }


      if (!$ap->settings) {
        $ap->settings = array(
            'class' => '',
            );
      }

      $fieldset = array();
      $fieldset['type_settings_time_groups'] = array(
          '#type' => 'select',
          '#title' => t('group time'),
          '#multiple' => false,
          '#options' => $pb_groups,
          '#default_value' =>  $ap->settings['class'],
          );

      return $fieldset;

    case 'validate':

      return;

    case 'submit':
      return array(
          array(
            'class' => $ap->type_settings_time_groups,
            ),
          NULL
          );
    default:

      return NULL;

  }

}


/**
* @author  Florian Wittmann
*/
function wisski_textproc_buildrankings($patterns) {

  if (empty($patterns) || !is_array($patterns)) return ""; // by Martin; necessary when settings array is empty
  $text = "";
  foreach ($patterns as $i => $value) {
    $text .= $value['rank'];
    $text .= " ";
    foreach ($value['states'] as $j => $state) {
      switch($state['type']) {
        case "givenname":
          $text .= "g";
        break;
        case "surname":
          $text .= "s";
        break;
        case "comma":
          $text .= "c";
        break;
        default:
        $err_message = t("Unknown statetype: ");
        $err_message .= $state['type'];
        drupal_set_message($err_message);
      }
      $text .= $state['occurence'];	

    }
    $text .= "\n";
  }	    
  return $text;
}


/**
* @author  Florian Wittmann
*/
function wisski_textproc_buildrankingsreverse($text) {
  $rankings = array();
  $lines = preg_split('/\r\n|\r|\n/', $text);
  foreach ($lines as $i => $line) {
    $parsedline = array();
    $line = trim($line);
    if (strlen($line) > 0) {
      $parts = explode(" ", $line, 2);
      $rank = 0 + $parts[0];
      $parsedline['rank'] = $rank;
      $typechars = str_split(trim($parts[1]));
      $lastchar = "";
      $states = array();
      foreach ($typechars as $j => $char) {	
        if ($lastchar != "") {
          if ($char == "*" or $char == "+" or $char == "?") {
            $states[] = wisski_textproc_charstoarray($lastchar,$char);
            $lastchar = "";
          } else {
            $states[] = wisski_textproc_charstoarray($lastchar,"");
            $lastchar = $char;
          }
        } else $lastchar = $char;

      }
      if($lastchar!="") $states[] = wisski_textproc_charstoarray($lastchar, "");
      $parsedline['states'] = $states;
      $rankings[] = $parsedline;
    }
  }
  return $rankings;
}

/**
* @author  Florian Wittmann
*/
function wisski_textproc_build_has_pos_reverse($text) {
  $lines = preg_split('/\r\n|\r|\n/', $text);
  foreach ($lines as $i => $line) {
    $parts = preg_split("/[\s,]+/", $line, -1, PREG_SPLIT_NO_EMPTY);
    if (count($parts)==1) {
      $rankings[''] = $parts[0];
    } elseif (count($parts)==2) {
      $rankings[$parts[0]] = $parts[1];
    }
  }
  return $rankings;
}
