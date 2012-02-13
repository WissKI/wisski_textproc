<?php



/**
* @author Florian Wittmann
*/
function wisski_textproc_getfromaid($aid) {
  $result = db_query("select settings from {wisski_textproc_pipes} where aid ='%s'", $aid);
  $settings = db_fetch_array($result);
  if ($settings != null) $settings = unserialize($settings['settings']);
  if ($settings == null) $settings = array();
  return $settings;
}


/**
* @author Martin Scholz 
*/
function wisski_textproc_admin_document_settings ($form_state) {

  $settings = variable_get('wisski_textproc_paths', array());
  $form = array();
  $form['doc_class'] = array(
      '#type' => 'textfield',
      '#title' => t('Group for documents'),
      '#default_value' => isset($settings['doc_class']) ? $settings['doc_class'] : '',
      );
  $form['creator'] = array(
      '#type' => 'textfield',
      '#title' => t('Path for creator of a document'),
      '#default_value' => isset($settings['creator']) ? $settings['creator'] : '',
      );
  $form['subject'] = array(
      '#type' => 'textfield',
      '#title' => t('Path for subject of a document'),
      '#default_value' => isset($settings['subject']) ? $settings['subject'] : '',
      );
  $form['refers_to'] = array(
      '#type' => 'textfield',
      '#title' => t('Path for referred objects'),
      '#default_value' => isset($settings['refers_to']) ? $settings['refers_to'] : '',
      );
  $form['default_doc_lang'] = array(
      '#type' => 'radios',
      '#title' => t('Default document language'),
      '#options' => array('de' => 'Deutsch', 'en' => 'English'),
      '#default_value' => variable_get('wisski_textproc_default_document_lang', 'de'),
      );

  $form['submit'] = array(
      '#type' => 'submit',
      '#value' => t('Save configuration'),
      );

  return $form;

}


/**
* @author Martin Scholz 
*/
function wisski_textproc_admin_document_settings_submit ($form, $form_state) {

  $settings = array();
  $settings['doc_class'] = $form_state['values']['doc_class'];
  $settings['creator'] = $form_state['values']['creator'];
  $settings['subject'] = $form_state['values']['subject'];
  $settings['refers_to'] = $form_state['values']['refers_to'];
  variable_set('wisski_textproc_paths', $settings);
  variable_set('wisski_textproc_default_document_lang', $form_state['values']['default_doc_lang']);

}


/**
* @author Florian Wittmann
*/
function wisski_textproc_admin_list($form_state) {
  $aps = wisski_textproc_get();
  $aps_list = array();
  foreach ($aps as $ap) {
    $aid = $ap->aid;


    $ops = '';
    $op_list = (array) module_invoke($ap->module, 'wisski_textproc', 'operations', $ap);
    foreach($op_list as $op_title => $op_url) {
      $ops .= '<a href="' . url($op_url) . '">' . $op_title . '</a> ';
    }

    $data = array(
        check_plain($ap->name),
        check_plain($ap->type),
        $ops
        );

    $form['rows'][$aid]['data']=array(
        '#type' => 'value',
        '#value' => $data
        );

    //now create the weight form element. 
    //NOTE how we add the id into the element key
    $form['rows'][$aid]['weight-'.$aid]=array(
        '#type'=>'textfield',
        '#size'=>5,
        '#default_value'=>$ap->weight,
        //add a specific class in here - we need this later
        '#attributes' => array('class'=>'weight'),
        );

  }

  //Don't forget the submit button
  $form['submit']=array(
      '#type'=>'submit',
      '#value'=>t('Save changes'),
      );


  return $form;

}


/**
* @author Florian Wittmann
*/
function wisski_textproc_admin_list_submit($form, &$form_state) {
  foreach($form_state['values'] as $key=>$data){
    //we are only interested in weight elements
    if (substr($key,0,6)=='weight'){
      //cunningly we have the DB id of the row in the element name
      $id = str_replace('weight-','',$key);
      db_query("UPDATE {wisski_textproc_pipes} SET weight=%d WHERE aid=%d",$data,$id);
    }
  }
  //optionally set the redirect value in form_submit ...
}

/**
* @author Florian Wittmann
*/
function theme_wisski_textproc_admin_list($form){

  //loop through each "row" in the table array
  $table_rows = array();
  if (isset($form['rows'])) {
    foreach($form['rows'] as $id => $row){
      //we are only interested in numeric keys
      if (intval($id)){ 
        $this_row = $row['data']['#value'];

        //Add the weight field to the row
        $this_row[] = drupal_render($form['rows'][$id]['weight-'.$id]);

        //Add the row to the array of rows
        $table_rows[] = array('data'=>$this_row, 'class'=>'draggable');
      }
    }
  }

  //Make sure the header count matches the column count
  $header=array(
      "Name","Type","Operations","Order"
      );

  $output = theme('table',$header,$table_rows,array('id'=>'textproc-table'));
  $output .= drupal_render($form);

  // Call add_tabledrag to add and setup the JS for us
  // The key thing here is the first param - the table ID
  // and the 4th param, the class of the form item which holds the weight
  drupal_add_tabledrag('textproc-table', 'order', 'sibling', 'weight');     

  return $output;
}



/**
* @author Martin Scholz
*/
function wisski_textproc_admin_add() {

  $form = array();
  $form['dl'] = array(
      '#type' => 'markup',
      '#prefix' => '<dl>',
      '#suffix' => '</dl>',
      );
  $types = wisski_textproc_get_types();
  uasort($types, function($a, $b) { return strcmp($a['title'], $b['title']); });
  foreach ($types as $type => $info) {
    if ($type == 'local') continue; // singleton!
    $form['dl'][$type] = array(
        '#type' => 'markup',
        '#prefix' => '<li>',
        '#value' => '<dt><a href="' . url('admin/settings/wisski/textproc/add/' . $type) . '">' . $info['title'] . '</a></dt><dd>' . $info['description'] . '</dd>',
        '#suffix' => '</li>',
        );
  }

  return $form;

}

/**
* @author Martin Scholz
*/
function wisski_textproc_admin_preprocessor() {

  $preprocessor = variable_get('wisski_textproc_preprocessor', array('de' => array('cmd' =>'', 'cwd' => ''), 'en' => array('cmd' => '', 'cwd' => ''), 'enabled' => false));

  $items['preprocessor_infotext'] = array(
      '#type' => 'markup',
      '#value' => t('A preprocessor may improve text analysis. The preprocessor can be defined as an external programm. It must read words line by line from stdin and print line by line lemma and part-of-speech tags, separated by a tab. Part-of-speech is optional.'),
      );
  $items['preprocessor_enabled'] = array(
      '#type' => 'checkbox',
      '#title' => t('enable preprocessor'),
      '#default_value' => $preprocessor['enabled'],
      );
  $items['cmd_de'] = array(
      '#type' => 'textfield',
      '#title' => t('Preprocessor command (German)'),
      '#description' => t('with optional arguments. data will be streamed to stdin and read from stdout.'),
      '#default_value' => $preprocessor['de']['cmd'],
      );
  $items['cwd_de'] = array(
      '#type' => 'textfield',
      '#title' => t('Working directory (German)'),
      '#default_value' => $preprocessor['de']['cwd'],
      );
  $items['cmd_en'] = array(
      '#type' => 'textfield',
      '#title' => t('Preprocessor command (English)'),
      '#description' => t('with optional arguments. data will be streamed to stdin and read from stdout.'),
      '#default_value' => $preprocessor['en']['cmd'],
      );
  $items['cwd_en'] = array(
      '#type' => 'textfield',
      '#title' => t('Working directory (English)'),
      '#default_value' => $preprocessor['en']['cwd'],
      );
  $items['submit'] = array(
      '#type' => 'submit',
      '#value' => t('Save'),
      '#submit' => array('wisski_textproc_admin_preprocessor_submit'),
      );
  return $items;

}

/**
* @author Florian Wittmann
*/
function wisski_textproc_admin_edit(&$form_state, $aid = NULL, $type = '') {

  $edit = $aid != NULL;

  $types = wisski_textproc_get_types();
  if (!isset($types[$type])) $type = '';

  if ($edit) {
    $ap = wisski_textproc_get($aid);
    if (!$type) $type = $ap->type;
  } else {
    $ap = (object) array(
        'aid' => NULL,
        'name' => '',
        'lang' => '',
        'profile' => 'default',
        'weight' => 0,
        'type' => $type,
        'module' => ($type) ? $types[$type]['module'] : '',
        'settings' => NULL,
        );
  }

  $types_opt = (!$type) ? array('' => '<' . t('Select type') . '>') : array();
  foreach ($types as $t => $info) {
    if ($t == 'local') continue;
    $types_opt[$t] = $info['id'];
  }

  // set multipart, in case the endpoint settings section has a file field
  $form['#attributes'] = array('enctype' => "multipart/form-data");

  // store aid, module and settings in hidden fields
  $form['aid'] = array('#type' => 'value', '#value' => $ap->aid);
  $form['module'] = array('#type' => 'value', '#value' => $ap->module);
  $form['settings'] = array('#type' => 'value', '#value' => $voc->settings);
  $form['lang'] = array('#type' => 'value', '#value' => $ap->lang);
  $form['weight'] = array('#type' => 'value', '#value' => $ap->weight);
  $form['profile'] = array('#type' => 'value', '#value' => $ap->profile);

  $form['name'] = array(
      '#type' => 'textfield',
      '#title' => t('Title'),
      '#default_value' => $ap->name,
      '#required' => TRUE,
      );
  $form['type'] = array(
      '#type' => 'select',
      '#title' => 'Type',
      '#options' => $types_opt,
      '#default_value' => $ap->type,
      '#required' => TRUE,
      '#disabled' => $edit,
      '#ahah' => array(
        'path' => 'wisski/textproc/js/edit',
        'wrapper' => 'type-settings-wrapper',
        'method' => 'replace',
        'effect' => 'fade',
        ),
      );

  $form['type_settings'] = array(
      '#type' => 'fieldset',
      '#title' => t('@type dependent settings', array('@type' => (!$ap->type) ? t('Type') : $types[$ap->type]['title'])),
      '#description' => ($type) ? $types[$type]['description'] : t('No type selected. Please select a type.'),
      '#prefix' => '<div id="type-settings-wrapper">',
      '#suffix' => '</div>',
      );

  $form['type_settings'] += (array) module_invoke($ap->module, 'wisski_textproc', 'edit', $ap);

  $form['buttons'] = array(
      '#prefix' => '<div>',
      '#suffix' => '</div>',
      );
  $form['buttons']['submit'] = array(
      '#type' => 'submit',
      '#value' => t('Save'),
      '#executes_submit_callback' => TRUE,
      '#validate' => array('wisski_textproc_admin_edit_validate'),
      '#submit' => array('wisski_textproc_admin_edit_submit'),
      );
  if ($edit) {
    $form['buttons']['delete'] = array(
        '#type' => 'submit',
        '#value' => t('Delete'),
        '#submit' => array('wisski_textproc_admin_edit_delete'),
        );
  }
  $form['buttons']['cancel'] = array(
      '#prefix' => '<a href="..">',
      '#value' => t('Cancel'),
      '#suffix' => '</a>',
      );

  return $form;

}


/**
* @author Florian Wittmann
*/
function wisski_textproc_admin_edit_validate($form, &$form_state) {
  $ap = (object) $form_state['values'];

  // name should be unique => check if name already exists
  $res = db_query("SELECT * FROM {wisski_textproc_pipes} WHERE `name` = '%s'", $ap->name);
  $other_ap = db_fetch_object($res);
  if ($other_ap && $other_ap->aid != $ap->aid) {
    form_set_error('name', 'There already exists an textprocessor with that name. Please choose a different one.');
  }

  $types = wisski_textproc_get_types();
  if ($types[$ap->type]['multiple'] != 1) {
    // Do not add multiple items of a type if not allowed
    $res = db_query("SELECT * FROM {wisski_textproc_pipes} WHERE `type` = '%s'", $ap->type);
    $other_ap = db_fetch_object($res);
    if ($other_ap && $other_ap->aid != $ap->aid) {
      form_set_error('type', 'There already exists an textprocessor with that type. Please choose a different one.');
    }
  }

  module_invoke($ap->module, 'wisski_textproc', 'validate', $ap);

}


/**
* @author Florian Wittmann
*/
function wisski_textproc_admin_edit_submit($form, &$form_state) {
  $ap = (object) $form_state['values'];

  list($settings, $redirect) = module_invoke($ap->module, 'wisski_textproc', 'submit', $ap);

  $ap->settings = $settings;

  wisski_textproc_save($ap);

  if ($redirect) $form_state['redirect'] = $redirect;
  else $form_state['redirect'] = 'admin/settings/wisski/textproc';

}


/**
* @author Martin Scholz
*/
function wisski_textproc_admin_preprocessor_submit($form, &$form_state) {
  // update preprocessor
  variable_set('wisski_textproc_preprocessor', array('de' => array('cmd' => $form_state['values']['cmd_de'], 'cwd' => $form_state['values']['cwd_de']), 'en' => array('cmd' => $form_state['values']['cmd_en'], 'cwd' => $form_state['values']['cwd_en']), 'enabled' => $form_state['values']['preprocessor_enabled']));
}



/**
* @author Florian Wittmann, Martin Scholz
*/
function wisski_textproc_admin_edit_js() {

  $form_state = array('storage' => NULL, 'submitted' => FALSE, 'rebuild' => TRUE);
  $form_build_id = $_POST['form_build_id'];
  $form = form_get_cache($form_build_id, $form_state);

  $args = $form['#parameters']; 
  $form_id = array_shift($args);  
  $form_state['values'] = $form_state['post'] = $form['#post'] = $_POST;  
  $form['#programmed'] = $form['#redirect'] = FALSE;
  $args[2] = $form_state['post']['type'];  // pass type to form hook as arg ($type is arg 1 in form hook; 1 must be added to args index)

  // we don't want to submit anything! don't call: drupal_process_form($form_id, $form, $form_state);
  $form = drupal_rebuild_form($form_id, $form_state, $args, $form_build_id);

  $settings = $form['type_settings'];
  unset($settings['#prefix'], $settings['#suffix']);

  $output = drupal_render($settings);

  print drupal_json(array('status' => true, 'data' => $output));
  exit();

}



/**
* @author Florian Wittmann
*/
function wisski_textproc_charstoarray($firstchar, $followingchar) {
  $charsarray = array();
  $charsarray['type'] = wisski_textproc_matchtype($firstchar);
  $charsarray['occurence'] = $followingchar;
  return $charsarray;
}

/**
* @author Florian Wittmann
*/
function wisski_textproc_matchtype($typechar) {
  switch ($typechar) {
    case "g":
      return "givenname";
    case "s":
      return "surname";
    case "c":
      return "comma";
    default:
    return "unknown";
  }
}


/**
* @author Florian Wittmann
*/
function wisski_textproc_admin_edit_delete($form, &$form_state) {

  $form_state['redirect'] = 'admin/settings/wisski/textproc/' . $form_state['values']['aid'] . '/delete';

}


/**
* @author Florian Wittmann
*/
function wisski_textproc_admin_delete($aid) {

  $ap = wisski_textproc_get($aid);

  return drupal_get_form(
      'wisski_helper_delete_template',
      $ap->name,
      "admin/settings/wisski/textproc/$aid/edit", 
      "admin/settings/wisski/textproc",
      'wisski_textproc_delete',
      $aid
      );

}

